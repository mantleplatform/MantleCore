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

/**
 * An additive material's Fresnel rim glow (F4), read from its graph: what the client's rim key draws, Color x pow(1 - max(0, N dot V), Power).
 *
 * Unreal's Fresnel is F = pow(1 - max(0, N dot V), Exponent) x (1 - BRF) + BRF (BRF: its base reflect fraction). The bake sees every surface
 * face-on (MaterialBaking's ExportMaterialProxy makes the camera vector the vertex normal), where F = BRF, so for an additive output of
 * Base + K x F the baked glow already holds Base + K x BRF. What it lacks is K x (1 - BRF) x pow(1 - N dot V, Exponent): Color is
 * K x (1 - BRF), and Power the Exponent. Additive adds emissive x opacity, so a Fresnel in either is read.
 */
struct MANTLEEDITOR_API FMantleRimGlow
{
	/** A rim the client can draw was read: Color and Power hold it. */
	bool bFound = false;

	/** Linear RGB at a grazing angle; a channel may pass 1 (the document's strength carries the part over 1). */
	double Color[3] = { 0.0, 0.0, 0.0 };

	/** The Fresnel's exponent; the document holds it to 0..MaxPower (schema/material.schema.json rim.power). */
	double Power = 5.0;
	static constexpr double MaxPower = 64.0;

	/** The Fresnel's Normal pin is wired (a normal map): the client's rim follows the mesh's own normals instead. */
	bool bNormalPin = false;

	/** Without bFound, why the rim cannot be published (it starts lower-case, to follow "because"); empty when the graph has no Fresnel. */
	FString Why;
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
	 * grows toward grazing angles. The bake sees the surface face-on, where a Fresnel is its base reflect fraction, so the
	 * baked glow holds only that much of it; ReadRim reads the rest for an additive material. Emissive may be null.
	 */
	static bool HasFresnel(const FExpressionInput* Emissive);

	/**
	 * F4: the Fresnel rim glow of an additive material whose emissive (Emissive) times opacity (Opacity, null or unconnected
	 * for 1) is Base + K x F, with F one Fresnel node (or several with the same exponent and base reflect fraction) and K a
	 * constant color: constants and Scalar/Vector Parameters as Context sets them, through Multiply, Divide, Add, Subtract,
	 * One Minus, Linear Interpolate and reroutes. The Base part (anything without the Fresnel, textures included) is the bake's.
	 * Out.bFound false with Out.Why set when a Fresnel is there but cannot be carried: it multiplies something that changes
	 * across the surface (a texture), its edges are darker than its face, it is inside a material function, it goes through a
	 * node not listed above, or its exponent or base reflect fraction is not a constant. False and Out.Why empty when neither
	 * graph has a Fresnel. Game thread (reads the editor-only graph).
	 */
	static bool ReadRim(const UMaterialInterface* Context, const FExpressionInput* Emissive, const FExpressionInput* Opacity, FMantleRimGlow& Out);
};

class UTexture;

/** One texture the emissive graph reads, as the material (or its instance) sets it. */
struct MANTLEEDITOR_API FMantleGlowTexture
{
	/** The texture, a parameter's override resolved: never null. */
	UTexture* Texture = nullptr;

	/** The Texture Sample Parameter's name when the texture comes from one, else none. */
	FName Parameter;

	/** The Scalar Parameters set to 0 that are multiplied with this texture's branch of the graph (its own "Brake Intensity"), and the ones shared with every texture beside it. */
	TArray<FName> OffScalars;
};

/** Where a material glows, for the vehicle builder's light finder: what its emissive graph reads, as the material (or its instance) sets it. */
struct MANTLEEDITOR_API FMantleMaterialGlow
{
	/** The emissive input is connected to a texture or to a color that is not black. */
	bool bGlows = false;

	/** The first texture on the emissive path (a Texture Sample, or a Texture Sample Parameter as the instance sets it); null for a glow of one color. Textures[0]'s texture. */
	UTexture* Texture = nullptr;

	/**
	 * Every distinct texture on the emissive path, in the graph's pre-order (a material with a lights, a brake and a reverse texture, switched by parameters, lists
	 * all three). A texture two nodes read is listed once. Empty for a glow of one color.
	 */
	TArray<FMantleGlowTexture> Textures;

	/** The first texture on the base color path: the lens's own color, for a glow texture that is only a white mask. Null when the base color has none. */
	UTexture* BaseTexture = nullptr;

	/** The first color on the path (a Vector Parameter before a constant), white when there is none. */
	FLinearColor Color = FLinearColor::White;

	/**
	 * The first color on the path is an exposed tint (a Vector Parameter) set to black: the glow is switched off by it, the way a glTF's Emissive Factor of 0 switches
	 * off the white default emissive texture every imported material carries. Meaningful with a glow that is only a flat color or a uniform texture.
	 */
	bool bColorOff = false;

	/**
	 * The Scalar Parameters on the path the material sets to 0: a glow switched off in the model ("Emissive Intensity" 0, the way the Fab cars ship). The glow is
	 * still where the texture says; a light made from it turns these on so it has something to glow with.
	 */
	TArray<FName> OffScalars;

	/**
	 * Reads Material's emissive graph, into the functions it calls by the output the emissive input reads (a glTF material is one call whose outputs read their own
	 * textures: only the emissive ones are on the path). False, with Out.bGlows false, when nothing is connected or the graph holds no texture and
	 * only black. Game thread (reads the material's editor-only graph).
	 */
	static bool Read(const UMaterialInterface* Material, FMantleMaterialGlow& Out);
};
