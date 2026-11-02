// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
struct FExpressionInput;

/**
 * #628: an unlit translucent material whose Emissive Color is the scene behind it (a Scene Color node on the emissive
 * path, usually sampled at a distorted screen position and tinted): the "fake refraction" a Fab pack uses for clear
 * glass and liquid. GLTFExporter bakes it on a quad with no scene behind it, where Scene Color is black, so the bake is
 * an opaque black solid. Mantle has no scene-color access in its place materials, so the surface publishes as what
 * the source reads as: tinted, see-through glass, a translucent surface with a low opacity and the tint as its glow.
 *
 * The tint is the first color (a Vector Parameter or a Constant3Vector) in the emissive graph, times the scalar it is
 * multiplied by (a constant or a Scalar Parameter, the nearest Multiply above it), both as the material sets them; a
 * graph with no color in it gets a neutral one. Nothing here depends on a name.
 */
struct MANTLEEDITOR_API FMantleSeeThroughGlass
{
	/** The emissive graph reaches a Scene Color node (the caller has seen the material is unlit and translucent). */
	bool bFound = false;

	/** A tint color was found in the graph (false: the neutral tint stands in). */
	bool bTint = false;

	/** The tint, linear RGB. White (neutral) when none was found. */
	double Tint[3] = { 1.0, 1.0, 1.0 };

	/** The scalar the tint is multiplied by (1 when the graph has none). */
	double Intensity = 1.0;

	/**
	 * The opacity the glass publishes with. The source draws the scene behind it in full, so the glass is meant to be
	 * seen through. Capsules are two-sided, so a view crosses two layers: at 0.15 about 72% of the scene shows through
	 * both.
	 */
	static constexpr double Opacity = 0.15;

	/**
	 * Scales the source's tint times intensity (the source multiplies the scene by the tint, often at twice the tint)
	 * down to a glow a translucent surface can carry without washing out what is behind it.
	 */
	static constexpr double GlowScale = 0.2;

	/** The glow the document publishes: Tint x Intensity x GlowScale per channel, 0..1. */
	void Glow(double (&Out)[3]) const;
};

/** Reads what an emissive graph holds beyond what GLTFExporter's bake can carry (#628). */
struct MANTLEEDITOR_API FMantleGlowMaterial
{
	/**
	 * Out.bFound when Emissive's graph (through reroutes, and into the functions it calls) reaches a Scene Color node,
	 * with the tint and its intensity read as FMantleSeeThroughGlass says. The caller has checked the material is unlit
	 * and translucent. Emissive may be null (then nothing is found).
	 */
	static bool RecognizeSceneColorGlass(const UMaterialInterface* Context, const FExpressionInput* Emissive, FMantleSeeThroughGlass& Out);

	/**
	 * Whether Emissive's graph holds a Fresnel (the Fresnel node, or a called function named for one): a rim glow that
	 * grows toward grazing angles. An additive material drops it and says so, rather than adding its average: the bake
	 * sees the surface face-on, where a Fresnel is 0, so the baked glow already holds none of it; a constant average
	 * would be a flat glow the creator never saw anywhere on the model; and over a sphere seen from afar the average is
	 * 2 / ((p + 1)(p + 2)) of the color it multiplies, 2% at the power of 8 a rim glow usually uses and 7% at 4.
	 * Emissive may be null.
	 */
	static bool HasFresnel(const FExpressionInput* Emissive);
};

class UTexture;

/** Where a material glows, for the vehicle builder's light finder: what its emissive graph reads, as the material (or its instance) sets it. */
struct MANTLEEDITOR_API FMantleMaterialGlow
{
	/** The emissive input is connected to a texture or to a color that is not black. */
	bool bGlows = false;

	/** The first texture on the emissive path (a Texture Sample, or a Texture Sample Parameter as the instance sets it); null for a glow of one color. */
	UTexture* Texture = nullptr;

	/** The first texture on the base color path: the lens's own color, for a glow texture that is only a white mask. Null when the base color has none. */
	UTexture* BaseTexture = nullptr;

	/** The first color on the path (a Vector Parameter before a constant), white when there is none. */
	FLinearColor Color = FLinearColor::White;

	/**
	 * The Scalar Parameters on the path the material sets to 0: a glow switched off in the model ("Emissive Intensity" 0, the way the Fab cars ship). The glow is
	 * still where the texture says; a light made from it turns these on so it has something to glow with.
	 */
	TArray<FName> OffScalars;

	/**
	 * Reads Material's emissive graph (its own, not into functions). False, with Out.bGlows false, when nothing is connected or the graph holds no texture and
	 * only black. Game thread (reads the material's editor-only graph).
	 */
	static bool Read(const UMaterialInterface* Material, FMantleMaterialGlow& Out);
};
