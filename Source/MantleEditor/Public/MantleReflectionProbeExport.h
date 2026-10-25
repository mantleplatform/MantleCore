// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class FMantleCoverage;
class UReflectionCaptureComponent;
struct FMantleValidationResult;

/**
 * What the exporter does with a placed Reflection Capture (#628): a Box or Sphere capture publishes as a
 * ReflectionProbe entity, which every client captures at run time in the place's own lighting
 * (FMantleReflectionProbes, MantleReflectionProbe.h). Nothing baked is carried, so the capture's baked data, if it
 * had any, is replaced rather than kept. A Plane capture is a separate planar system with no place form: it is left
 * out, with a line in the coverage report (FMantleActorDecompose::ReportIgnoredActor).
 *
 * The shared exporter, validator and survey files only call this.
 */
class MANTLEEDITOR_API FMantleReflectionProbeExport
{
public:
	/** The Box or Sphere capture component of a placed Reflection Capture actor, or null for anything else (a Plane capture included). */
	static const UReflectionCaptureComponent* MappableCapture(const AActor* Actor);

	/** Whether Actor is a placed Box or Sphere Reflection Capture, which publishes as a ReflectionProbe. */
	static bool IsProbeActor(const AActor* Actor);

	/**
	 * Reads Actor's capture into Out, inside the limits a place holds: a box's half extent is the capture's world scale
	 * (the engine's box is the unit box scaled by it), its transition distance, a sphere's influence radius, the
	 * brightness and the sky light leak (the mean of RuntimeSkylightScale's channels). A number outside the schema's
	 * range is clamped and said so in OutApproximations. Out is untouched for an actor that is not a probe.
	 */
	static void Read(const AActor* Actor, FMantleReflectionProbeProps& Out, TArray<FString>& OutApproximations);

	/**
	 * The transform the entity is written with: the capture's world location with its CaptureOffset folded in (the
	 * client captures from the entity's location, which is also where the influence volume is centred) and its
	 * rotation, at scale 1 (the box's size is in the properties). Relative to ExportedParent's actor transform when
	 * the entity has a parent.
	 */
	static FTransform PublishedTransform(const AActor* Actor, const AActor* ExportedParent);

	/** Writes Entity's properties and transform from Actor (FMantlePlaceExporter::ExtractProperties' case for a probe). */
	static void Extract(const AActor* Actor, FMantleEntity& Entity);

	/**
	 * Export's pass 1 for a probe that classified: whether it publishes. Reports every outcome on Coverage under
	 * Lights: Kept; Approximated ("captured at runtime with the place's own lighting") for a capture that had baked
	 * data or a specified cubemap, an offset folded into its position, or a size clamped; Dropped, with the reason, for
	 * one on a target client that does not read probes (FMantleTargetFeatures), a hidden one, and every one past
	 * FMantlePlaceLimits::MaxReflectionProbesPerPlace. InOutAdmitted counts the ones published so far.
	 */
	static bool AdmitForExport(const AActor* Actor, int32& InOutAdmitted, FMantleCoverage& Coverage);

	/**
	 * Check this place's note for a probe (not an error: the exporter leaves out what a place cannot hold). Warns for
	 * a probe past the per-place cap, whose size is clamped, or that is hidden. Does nothing for another Type.
	 */
	static void Check(const AActor* Actor, EMantleEntityType Type, int32& InOutProbes, FMantleValidationResult& Result);
};
