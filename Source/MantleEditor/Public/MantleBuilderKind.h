// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBodyMeasurements.h"
#include "MantleBuilderPoint.h"

/** How the viewport draws a point, so a creator sees what it is without reading a name. */
enum class EMantleBuilderPointShape : uint8
{
	Hand,
	Foot,
	Seat,
	/** The muzzle, a sight line, an exit direction, a hinge axis. */
	Arrow,
	Dot
};

/** One kind of point a builder places, in plain words for a creator who does not know Unreal. */
struct FMantleBuilderPointRole
{
	FName Role;

	/** Plain words, e.g. "Right hand". */
	FText Label;

	/** One plain sentence on what the point does and what a mistake looks like. */
	FText Help;

	EMantleBuilderPointShape Shape = EMantleBuilderPointShape::Dot;
	bool bRequired = true;

	/** False: only the location matters, the gizmo shows no rotation. */
	bool bRotates = true;

	/** True for exits: only the turn about up matters. */
	bool bYawOnly = false;

	FName DefaultParent;

	/** The parents this role may have; empty is any. */
	TArray<FName> AllowedParents;

	/** The role this mirrors into across the mesh's centre line (Hand_L and Hand_R, exits, driver and passenger); NAME_None for none. */
	FName MirrorRole;
};

/** An oriented box a kind asks the viewport to draw (e.g. a palm), in the mesh's own unscaled space; red when bWarning. */
struct FMantleBuilderOverlayBox
{
	FName Part;
	FVector Center = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector HalfExtent = FVector(1.0);
	bool bWarning = false;
};

/** A line a kind asks the viewport to draw, in the mesh's own unscaled space. */
struct FMantleBuilderOverlayLine
{
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
};

/** A small labelled marker (a sphere and a name) a kind asks the viewport to draw, in the mesh's own unscaled space. */
struct FMantleBuilderOverlayMarker
{
	FVector Location = FVector::ZeroVector;
	FText Label;
	FLinearColor Color = FLinearColor::White;
	float RadiusCm = 2.f;
};

/** Extra things a kind wants drawn on top of the points and the ghost (the vehicle builder labels its seats and the vehicle's weight). Empty by default, so a kind that has none is unaffected. */
struct FMantleBuilderOverlay
{
	TArray<FMantleBuilderOverlayBox> Boxes;
	TArray<FMantleBuilderOverlayLine> Lines;
	TArray<FMantleBuilderOverlayMarker> Markers;
	/** Wire capsules (a hand, say), drawn like the ghost; a zero-length one is a sphere. Red when bWarning. */
	TArray<FMantleGhostCapsule> Capsules;
};

/**
 * What a builder is for: the weapon builder and the vehicle builder each implement one, and share the point type, the
 * body measurements, the viewport and the merge, mirror and reset rules (#643, #729, #728).
 */
class MANTLEEDITOR_API IMantleBuilderKind
{
public:
	virtual ~IMantleBuilderKind() = default;

	virtual TArray<FMantleBuilderPointRole> GetRoles() const = 0;

	/** The automatic guess for every role, with Transform and Guess both set to it and Source = Guess. */
	virtual TArray<FMantleBuilderPoint> GuessPoints(const UObject* Mesh) const = 0;

	/** Moves a placement the creator proposes to somewhere sensible (e.g. onto the mesh surface); the default accepts it as is. */
	virtual FTransform Snap(FName Role, const FTransform& Proposed, const UObject* Mesh) const { return Proposed; }

	/** The character ghost for these points, in character space; capsules are flagged bWarning when e.g. a hand cannot reach. */
	virtual TArray<FMantleGhostCapsule> GhostPose(const TArray<FMantleBuilderPoint>& Points) const = 0;

	/** How the picture is turned, from mesh space to the view: the model, its points and the ghost all turn by it, so the body
	 *  holding the model stands upright. Points stay in mesh space. */
	virtual FQuat ViewTurn() const { return FQuat::Identity; }

	/** Plain-language warnings, never refusals. */
	virtual TArray<FText> Check(const TArray<FMantleBuilderPoint>& Points) const { return {}; }

	/** Extra boxes, lines and markers to draw for these points (mesh space); the default draws nothing. */
	virtual FMantleBuilderOverlay Overlay(const TArray<FMantleBuilderPoint>& Points) const { return FMantleBuilderOverlay(); }

	/** The line of help at the bottom of the picture; empty keeps the viewport's own (how to drag a point). A kind with no points says something else. */
	virtual FText Hint() const { return FText::GetEmpty(); }
};

namespace MantleBuilderPoints
{
	/**
	 * Creator points win over guesses (the same precedence as MantleContactPoints::ResolveContact: override over computed).
	 * Every point takes the new Guess; Transform follows the new guess only where Source is Guess; roles missing from Existing
	 * are added as guesses; Existing roles absent from NewGuesses are kept unchanged. A new point's guess is its Guess field.
	 */
	MANTLEEDITOR_API TArray<FMantleBuilderPoint> MergeGuesses(const TArray<FMantleBuilderPoint>& Existing, const TArray<FMantleBuilderPoint>& NewGuesses);

	/**
	 * Mirrors a point across the mesh's Y = 0 plane into its MirrorRole: the location's y is negated and the rotation reflected
	 * (a right-hand frame becomes the matching left-hand frame, handedness kept, so a yaw of 30 becomes -30). Source = Creator.
	 * False when the role has no MirrorRole.
	 */
	MANTLEEDITOR_API bool MirrorPoint(const FMantleBuilderPoint& From, const TArray<FMantleBuilderPointRole>& Roles, FMantleBuilderPoint& OutMirrored);

	/** Every point back to its guess. */
	MANTLEEDITOR_API void ResetAll(TArray<FMantleBuilderPoint>& Points);

	/** The point for a role, or null. */
	MANTLEEDITOR_API const FMantleBuilderPoint* Find(const TArray<FMantleBuilderPoint>& Points, FName Role);
}
