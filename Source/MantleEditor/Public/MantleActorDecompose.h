// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCoverage.h"
#include "MantlePlaceTypes.h"

class AActor;
class FJsonObject;
class UActorComponent;
class UEdGraphNode;
class UStaticMesh;

/**
 * A static mesh instance with a negative scale, as it publishes (#628): the mirrored copy of its mesh and the
 * positive scale to place the copy at (FMantleMirrorBake), or why that could not be made.
 */
struct FMantleMirrorResult
{
	/** The scale has a negative axis and the component has a mesh: everything below applies. */
	bool bNeeded = false;

	/** The copy was made. False with bNeeded set means the instance is left out, and Why says why. */
	bool bOk = false;

	/** The cached transient copy (FMantleMirrorBake keeps it alive); null unless bOk. */
	UStaticMesh* Mesh = nullptr;

	/** abs(scale): what the published transform carries in place of the negative one. */
	FVector PositiveScale = FVector::OneVector;

	FString Why;
};

/** What one Blueprint class of an actor's hierarchy does at run time: the wired events and functions of its graphs. */
struct FMantleBlueprintBehaviour
{
	FString Blueprint;
	TArray<FString> Events;
	TArray<FString> Functions;
};

/**
 * What the exporter does with an actor made of several components, and says about what it cannot carry (#628).
 * FMantlePlaceExporter::CollectPrefabParts decomposes such an actor (a Blueprint prefab: a fridge's body, doors
 * and lights) into a Group with one child entity per component; this holds the pieces of that which need no
 * exporter internals: the mirrored copies for negative scales, the Effect properties a particle component
 * translates to, the cheap honest description of a Blueprint's behaviour, and the coverage lines for whatever
 * is ignored or dropped. The shared exporter, validator and survey files only call it.
 */
class MANTLEEDITOR_API FMantleActorDecompose
{
public:
	// ---- Mirrored copies ----

	/** Whether Scale mirrors a mesh: any axis below zero. */
	static bool HasNegativeAxis(const FVector& Scale);

	/** The mirrored copy of Source for Scale (FMantleMirrorBake, cached, so Gather, Check and Export agree). Not needed for a null mesh or a scale above zero. */
	static FMantleMirrorResult ResolveMirror(UStaticMesh* Source, const FVector& Scale);

	/** ResolveMirror for a static mesh actor, by the scale Export publishes it at (FMantlePlaceExporter::PublishedTransform). Not needed for anything else. */
	static FMantleMirrorResult MirrorForActor(const AActor* Actor);

	// ---- Effects ----

	/**
	 * Reads one Effect "properties" object, as FMantleEffectExport::Translate returns it, into Out
	 * (FMantleEffectExport::ReadProperties): archetype (required), color [r, g, b], size, rate, speed, lifetime,
	 * autoplay and loop, and for sprites every sprites key; a key left out keeps the archetype's own default. A
	 * sprites "texture" is kept as written in Out.TextureId: an object path (FMantleEffectExport::IsTexturePath)
	 * that the exporter turns into the uploaded texture's id. False when the archetype is missing or not a
	 * built-in one. Unknown keys are ignored.
	 */
	static bool EffectPropsFromJson(const FJsonObject& Properties, FMantleEffectProps& Out);

	// ---- Blueprint behaviour ----

	/** Whether Node is an event of a Blueprint graph (an event, a custom event, a bound event, an input event). */
	static bool IsEventNode(const UEdGraphNode* Node);

	/** The event's name as a creator knows it ("BeginPlay", "Tick", "ActorBeginOverlap", or a custom event's own). */
	static FString EventNameOf(const UEdGraphNode* Node);

	/**
	 * The behaviour a Blueprint class hierarchy would run: for each Blueprint of Actor's class chain, child first, the
	 * wired events of its event graphs and the wired functions of its function graphs. The construction script is not
	 * counted: it ran in the editor, and what it made is in the level. False, with Out empty, when there is nothing.
	 */
	static bool CollectBlueprintBehaviour(const AActor* Actor, TArray<FMantleBlueprintBehaviour>& Out);

	/** "BeginPlay, Tick from parent BP_BaseRotator": the events (or functions) of each Blueprint with some, a parent's marked as such. Pure. */
	static FString ComposeBlueprintHint(const TArray<FMantleBlueprintBehaviour>& Levels);

	/**
	 * OutBlueprint is the actor's own Blueprint class, OutDetail the coverage line's text: "its behaviour isn't
	 * carried (e.g. <hint>)". False when the actor's class has no Blueprint behaviour.
	 */
	static bool DescribeBlueprintBehaviour(const AActor* Actor, FString& OutBlueprint, FString& OutDetail);

	// ---- Ignored and dropped ----

	/** A placed Box, Sphere or Plane Reflection Capture. The Box and Sphere ones publish as a ReflectionProbe (FMantleReflectionProbeExport). */
	static bool IsReflectionCapture(const AActor* Actor);

	/** A placed Reflection Capture that has no place form, a Plane one (planar reflections): Export leaves it out, and ReportIgnoredActor says so. */
	static bool IsUnpublishedReflectionCapture(const AActor* Actor);

	/** Editor-only visualisation: an editor-only component, a billboard or an arrow. */
	static bool IsVisualisation(const UActorComponent* Component);

	/** Reports an actor Export leaves out on purpose (a reflection capture with no place form, IsUnpublishedReflectionCapture) as Dropped. True when it was one. */
	static bool ReportIgnoredActor(const AActor* Actor, FMantleCoverage& Coverage);

	/**
	 * What an exported actor of Type loses that no entity says: its Blueprint behaviour (Dropped, one line per Blueprint
	 * class), the editor-only icons of a Blueprint actor (Info, one line per class), and, for a Group made of parts, every
	 * component that is neither a part nor content a player sees (movement, camera, child actor...) as Dropped by class.
	 */
	static void ReportActor(const AActor* Actor, EMantleEntityType Type, FMantleCoverage& Coverage);
};
