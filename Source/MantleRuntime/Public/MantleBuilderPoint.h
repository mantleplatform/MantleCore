// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBuilderPoint.generated.h"

/** Where a builder point's transform came from: the builder's own guess, or a placement the creator made. */
UENUM()
enum class EMantleBuilderPointSource : uint8
{
	Guess,
	Creator
};

/**
 * One placed point on a creator's mesh, shared by the weapon builder (grips, muzzle, sight, magazine) and the vehicle builder
 * (seats, hands on the wheel, feet, exits, door hinges). The kind decides what the Role names mean; this type only stores them.
 *
 * Space and scale: a point lives in its Parent's space in the SOURCE mesh's own unscaled units. So changing the builder's uniform
 * fit scale never moves a point relative to the mesh. Export multiplies the point locations by the fit scale; the editor preview
 * shows the unscaled mesh with the character ghost drawn at 1/scale, which is the character's true size relative to the fitted mesh.
 *
 * Parent accepts a bone, a socket or an attachment-part name of the source mesh; NAME_None is the mesh root. A kind may restrict
 * which parents a role allows.
 *
 * Creator points win over guesses: the builder keeps Transform when Source is Creator, and only refreshes Guess.
 */
USTRUCT()
struct MANTLERUNTIME_API FMantleBuilderPoint
{
	GENERATED_BODY()

	/** What the point is for, e.g. Hand_R. The kind defines the names. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FName Role;

	/** A bone, a socket or an attachment-part name of the source mesh; NAME_None is the mesh root. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FName Parent;

	/** In Parent space, in the source mesh's own unscaled units. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FTransform Transform;

	/** The builder's last automatic guess, in the same space. */
	UPROPERTY(EditAnywhere, Category = "Mantle")
	FTransform Guess;

	UPROPERTY(EditAnywhere, Category = "Mantle")
	EMantleBuilderPointSource Source = EMantleBuilderPointSource::Guess;

	/** Back to the builder's guess: Transform = Guess, Source = Guess. */
	void ResetToGuess()
	{
		Transform = Guess;
		Source = EMantleBuilderPointSource::Guess;
	}
};
