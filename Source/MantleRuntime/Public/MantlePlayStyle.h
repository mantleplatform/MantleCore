// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #232 play styles: the one description of a player's camera and movement style, shared by the place file
 * (settings.playStyle), Player:SetPlayStyle and the character that replicates it, so the option rules exist once.
 *
 * Default is Mantle's normal first and third person play. TopDown is a high camera over the character with
 * screen-relative movement. The side-scrolling style (#229) adds its own name and options here.
 *
 * The rules are applied one option at a time (ApplyNumber / ApplyFlag) and then checked together (Finish), so a
 * JSON reader and a script table reader report the same errors with the same words, each naming the option.
 */
enum class EMantlePlayStyle : uint8
{
	Default,
	TopDown
};

/** The TopDown options (#232), every one with its default. Lengths are cm, angles degrees. */
struct FMantleTopDownOptions
{
	float Distance = 1400.0f;
	float MinDistance = 800.0f;
	float MaxDistance = 2400.0f;
	/** Degrees the camera looks down. */
	float Pitch = 55.0f;
	/** The world heading screen-up points along; stored wrapped to -180..180. */
	float Yaw = 0.0f;
	float FieldOfView = 60.0f;
	/** Stage 2: the middle-mouse-button walk-there. Accepted and reported now. */
	bool bClickToMove = false;
	/** Stage 2: hide what stands between the camera and the character. Accepted and reported now. */
	bool bHideWalls = true;

	bool operator==(const FMantleTopDownOptions& Other) const
	{
		return Distance == Other.Distance && MinDistance == Other.MinDistance && MaxDistance == Other.MaxDistance
			&& Pitch == Other.Pitch && Yaw == Other.Yaw && FieldOfView == Other.FieldOfView
			&& bClickToMove == Other.bClickToMove && bHideWalls == Other.bHideWalls;
	}
	bool operator!=(const FMantleTopDownOptions& Other) const { return !(*this == Other); }
};

/** A style and its options. Default-constructed it is Default, which has none. */
struct FMantlePlayStyle
{
	EMantlePlayStyle Style = EMantlePlayStyle::Default;
	/** Meaningful only while Style is TopDown. */
	FMantleTopDownOptions TopDown;

	bool operator==(const FMantlePlayStyle& Other) const
	{
		return Style == Other.Style && (Style != EMantlePlayStyle::TopDown || TopDown == Other.TopDown);
	}
	bool operator!=(const FMantlePlayStyle& Other) const { return !(*this == Other); }
};

namespace MantlePlayStyle
{
	constexpr float MinDistanceLimit = 400.0f;
	constexpr float MaxDistanceLimit = 5000.0f;
	constexpr float MinPitch = 20.0f;
	constexpr float MaxPitch = 89.0f;
	constexpr float MinFieldOfView = 30.0f;
	constexpr float MaxFieldOfView = 90.0f;

	/** "Default" or "TopDown". */
	MANTLERUNTIME_API const TCHAR* StyleName(EMantlePlayStyle Style);
	/** False for any other text (names are case sensitive). */
	MANTLERUNTIME_API bool ParseStyleName(const FString& Name, EMantlePlayStyle& OutStyle);

	/** The TopDown options' names, in the order the docs list them. */
	MANTLERUNTIME_API TConstArrayView<const TCHAR*> TopDownOptionNames();
	MANTLERUNTIME_API bool IsNumberOption(const FString& Key);
	MANTLERUNTIME_API bool IsFlagOption(const FString& Key);

	/**
	 * Sets a numeric TopDown option. False with OutError ("Pitch must be from 20 to 89") for a value that is not finite or
	 * is out of its range; Yaw takes any finite number and is wrapped. Key must pass IsNumberOption.
	 */
	MANTLERUNTIME_API bool ApplyNumber(FMantleTopDownOptions& Options, const FString& Key, double Value, FString& OutError);
	/** Sets ClickToMove or HideWalls. Key must pass IsFlagOption. */
	MANTLERUNTIME_API void ApplyFlag(FMantleTopDownOptions& Options, const FString& Key, bool bValue);

	/** The checks across options: MinDistance not above MaxDistance, Distance between them. Names the option at fault in OutError. */
	MANTLERUNTIME_API bool Finish(const FMantleTopDownOptions& Options, FString& OutError);
}
