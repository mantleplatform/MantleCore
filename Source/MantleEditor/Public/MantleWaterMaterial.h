// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UMaterialInterface;
class UTexture;
struct FExpressionInput;

/**
 * One of a water's normal maps (#628: the document's water.normal and water.normal2): the source texture, and how it
 * is laid out and moves. Read from the Normal output's texture samples (the Spruce Forest river: two panned samples on
 * the first UV set) or from a called function that lays one out by world position (its lake: World Position XY over a
 * scale, plus Time times a speed).
 */
struct MANTLEEDITOR_API FMantleWaterNormalLayer
{
	TWeakObjectPtr<UTexture> Texture;

	/** Laid out by the world's X and Y (WorldSizeCm a repeat) rather than the mesh's first UV set (Tiling repeats across it). */
	bool bWorld = false;
	double Tiling[2] = { 1.0, 1.0 };
	double WorldSizeCm = 0.0;

	/** Repeats a second, added after the layout as frac(time x speed). */
	double Speed[2] = { 0.0, 0.0 };

	/** The strength a called function multiplies its own normal by (1 for a plain sample): carried into the normal scale. */
	double Strength = 1.0;

	/** A world layout the document cannot say exactly (a different scale on each axis, a mirrored axis, a size out of range). */
	bool bApproximated = false;

	bool Moves() const { return Speed[0] != 0.0 || Speed[1] != 0.0; }
};

/** The scattering and absorption coefficients (1/cm, linear RGB) the water key carries, from FMantleWaterMaterial::Coefficients. */
struct MANTLEEDITOR_API FMantleWaterCoefficients
{
	double Scattering[3] = { 0.0, 0.0, 0.0 };
	double Absorption[3] = { 0.0, 0.0, 0.0 };
};

/**
 * #628: a translucent material whose opacity is a depth fade (Depth Fade, Scene Depth minus Pixel Depth, or the
 * engine's CameraDepthFade), which is how a Fab river or lake is made. GLTFExporter bakes it on a quad with no
 * scene behind it, where the fade is at its near end, so the baked opacity is 0 and the water is invisible. The
 * far end of the fade (deep water, far from the camera) is what the surface looks like; this reads it from the
 * graph, so the document carries it as one constant opacity.
 */
struct MANTLEEDITOR_API FMantleWaterBake
{
	/** Opacity reaches a depth fade, or the material's shading model is Single Layer Water (bSingleLayer). */
	bool bFound = false;

	/**
	 * The material is already Single Layer Water (a Water plugin body's material, GhzGarage/mantle#818): opaque in blend mode, with no
	 * depth fade to read. Opacity is 1 and read, so it publishes with the water key; the colour and normals are read as for a fade.
	 */
	bool bSingleLayer = false;

	/** A Single Layer Water material animated its waves (world position offset): they are dropped, the water keeps its flat surface and its normal maps. */
	bool bWavesDropped = false;

	/** The Single Layer Water output node's scattering and absorption (1/cm), when its graph evaluated; they stand in for the derived coefficients. */
	bool bVolume = false;
	FMantleWaterCoefficients Volume;

	/** The opacity at the far end of the fade, 0..1: read from the graph, or EstimatedOpacity when the graph could not be evaluated. */
	double Opacity = 0.0;

	/** Opacity is what the graph gives; false means the estimate stands in (Why says what stopped the read). */
	bool bOpacityRead = false;

	/** The base colour at the far end of the fade (the deep colour), linear 0..1; set only when the graph evaluated to a colour. */
	bool bColour = false;
	double Colour[3] = { 0.0, 0.0, 0.0 };

	/**
	 * The water's panned normal as a document without the water key can carry it (translucent water, or a client that
	 * cannot read the key): the source normal texture, its tiling on the first UV set, and its pan in UV units a
	 * second, taken from the Panners that feed the Normal output's texture samples (through FlattenNormal and
	 * BlendAngleCorrectedNormals). Several samples publish as the fastest one; a sample laid out by world position is
	 * not one. Set only when a sample really pans; NormalSamples counts the samples found (Normals has them all).
	 */
	bool bNormalPanned = false;
	TWeakObjectPtr<UTexture> NormalTexture;
	double NormalTiling[2] = { 1.0, 1.0 };
	double NormalSpeed[2] = { 0.0, 0.0 };
	int32 NormalSamples = 0;

	/**
	 * Every normal sample the Normal output reaches, in the graph's order (the base normal first, then the one
	 * blended over it), still or moving, on the first UV set or by world position: what the water key carries as
	 * water.normal and water.normal2 (the two a document holds; NormalSamples counts any more). Empty when the
	 * Normal output was not read.
	 */
	TArray<FMantleWaterNormalLayer> Normals;

	/**
	 * FlattenNormal's strength on the way to the output (1 minus its Flatness; 1 when there is none or it is not a
	 * number), times the first layer's own function strength (Normals[0].Strength): the document's normalScale.
	 */
	double NormalScale = 1.0;

	/** The depth fade's fade distance, in centimetres, when the graph has a Depth Fade node and its distance could be read. */
	bool bFadeDistance = false;
	double FadeDistanceCm = 0.0;

	/** For the log: the first node that stopped an evaluation. */
	FString Why;

	/**
	 * A far-end opacity at or above this (read from the graph) publishes the water opaque: Mantle's translucent
	 * materials get no screen-space or Lumen reflections, so a near-opaque river would render as a flat dark colour.
	 */
	static constexpr double OpaqueAtOrAbove = 0.9;

	bool PublishesOpaque() const { return bFound && bOpacityRead && Opacity >= OpaqueAtOrAbove; }
};

/** Reads a depth-faded translucent material's far end (FMantleWaterBake). */
struct MANTLEEDITOR_API FMantleWaterMaterial
{
	/** Published when the opacity graph cannot be evaluated but plainly fades by depth: mostly see-through water that still reads as a surface. */
	static constexpr double EstimatedOpacity = 0.75;

	/** The opacity below which a read is not believed: a depth fade whose far end is clear is not a fade worth keeping. */
	static constexpr double MinimumOpacity = 0.02;

	/**
	 * Out.bFound when Opacity's graph reaches a depth fade (the caller has seen the material is translucent). The
	 * graph is then evaluated with every depth fade at its far end (faded fully in: Depth Fade gives its
	 * opacity input, Scene Depth is far and Pixel Depth near, CameraDepthFade is 1) over the nodes constants,
	 * parameters (as Context sets them), Add, Subtract, Multiply, Divide, Min, Max, Lerp, Clamp, Saturate,
	 * OneMinus, Abs, Power, Component Mask, Append, reroutes and static switches; anything else (a texture
	 * sample, Time, a function) stops that read. BaseColor is read the same way for the deep colour. Normal (may be
	 * null) is read for its panned samples (FMantleWaterBake::bNormalPanned).
	 */
	static bool Recognise(const UMaterialInterface* Context, const FExpressionInput* Opacity, const FExpressionInput* BaseColor, const FExpressionInput* Normal, FMantleWaterBake& Out);

	/**
	 * Out.bFound for a material whose shading model is Single Layer Water (GhzGarage/mantle#818): it is water already, so there is no
	 * fade to find; opacity is 1 and the water publishes opaque with the water key. BaseColor and Normal (either may be null: a material
	 * built with Set Material Attributes has no roots) are read as Recognise reads them, and the Single Layer Water output node's
	 * absorption and scattering, when its graph evaluates, become Out.Volume. What the graph does not give, the Water plugin's named
	 * parameters do ("Absorption", "Scattering", "Water Albedo", as the instance chain sets them), and the colour falls back to
	 * ClearLakeColour, so the water always has a volume and a surface colour.
	 */
	static bool RecogniseSingleLayer(const UMaterialInterface* Context, const FExpressionInput* BaseColor, const FExpressionInput* Normal, FMantleWaterBake& Out);

	/** The deep colour a Single Layer Water material gets when neither its graph nor its parameters give one: a clear lake's teal (linear RGB). */
	static constexpr double ClearLakeColour[3] = { 0.02, 0.08, 0.10 };

	/** The fade distance the coefficients assume when the source has none (a Scene Depth fade, Camera Depth Fade). */
	static constexpr double DefaultFadeDistanceCm = 100.0;

	/**
	 * The water key's scattering and absorption (1/cm) from a deep colour C (linear RGB, 0..1) and a fade distance D (cm;
	 * DefaultFadeDistanceCm when D is not above 0). A starting point, not a measurement: the source's fade is a
	 * see-through edge and Single Layer Water's volume is physical, so the two are tied by "about three
	 * extinction lengths across the fade distance", and the deep colour is the volume's albedo:
	 *   sigma = min(3 / D, 1)            the extinction (the schema's coefficients stop at 1)
	 *   albedo_c = clamp(C_c, 0, 0.9)    the deep colour itself: a boosted albedo turned the Spruce river's dark
	 *                                    teal (0.024) into bright cyan, where the editor shows it near black
	 *   scattering_c = sigma * albedo_c  absorption_c = sigma * (1 - albedo_c)
	 */
	static FMantleWaterCoefficients Coefficients(const double (&Colour)[3], double FadeDistanceCm);
};
