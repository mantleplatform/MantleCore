// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #232 play styles: the one description of a player's camera and movement style, shared by the place file
 * (settings.playStyle), Player:SetPlayStyle and the character that replicates it, so the option rules exist once.
 *
 * Default is Mantle's normal first and third person play. TopDown is a high camera over the character with
 * screen-relative movement. SideScroll (#229) is a side-on camera that follows the character along a flat path, with
 * movement held to that path.
 *
 * The rules are applied one option at a time (ApplyNumber / ApplyFlag) and then checked together (Finish), so a
 * JSON reader and a script table reader report the same errors with the same words, each naming the option.
 */
enum class EMantlePlayStyle : uint8
{
	Default,
	TopDown,
	SideScroll
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

/** The SideScroll options (#229), every one with its default. Lengths are cm, angles degrees. */
struct FMantleSideScrollOptions
{
	/**
	 * The path the character is held to: 2 to MantlePlayStyle::MaxPathPoints flat points (a script's Z is dropped) joined by straight
	 * segments, none shorter than MantlePlayStyle::MinSegmentLength. Empty: the line along +X through where the character stands
	 * when the style is applied (the character, not this struct, remembers that spot).
	 */
	TArray<FVector2D> Path;
	/** How far the camera sits from the path. */
	float Distance = 1000.0f;
	/** How far above the character the camera looks (it never tilts). */
	float Height = 100.0f;
	/** How far ahead of the character, in the way it faces, the camera aims. */
	float LookAhead = 150.0f;
	float FieldOfView = 65.0f;
	/** The camera stops at BoundsMin and BoundsMax, cm along the Path from its first point. Only with a Path. */
	bool bHasBounds = false;
	float BoundsMin = 0.0f;
	float BoundsMax = 0.0f;

	bool operator==(const FMantleSideScrollOptions& Other) const
	{
		return Path == Other.Path && Distance == Other.Distance && Height == Other.Height && LookAhead == Other.LookAhead
			&& FieldOfView == Other.FieldOfView && bHasBounds == Other.bHasBounds
			&& (!bHasBounds || (BoundsMin == Other.BoundsMin && BoundsMax == Other.BoundsMax));
	}
	bool operator!=(const FMantleSideScrollOptions& Other) const { return !(*this == Other); }
};

/** A style and its options. Default-constructed it is Default, which has none. */
struct FMantlePlayStyle
{
	EMantlePlayStyle Style = EMantlePlayStyle::Default;
	/** Meaningful only while Style is TopDown. */
	FMantleTopDownOptions TopDown;
	/** Meaningful only while Style is SideScroll. */
	FMantleSideScrollOptions SideScroll;

	bool operator==(const FMantlePlayStyle& Other) const
	{
		return Style == Other.Style && (Style != EMantlePlayStyle::TopDown || TopDown == Other.TopDown)
			&& (Style != EMantlePlayStyle::SideScroll || SideScroll == Other.SideScroll);
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

	// ---- SideScroll (#229) -------------------------------------------------------------------------------------------------------

	constexpr int32 MinPathPoints = 2;
	constexpr int32 MaxPathPoints = 64;
	/** A path segment shorter than this (flat, cm) is an error. */
	constexpr float MinSegmentLength = 50.0f;
	constexpr float MinSideDistance = 300.0f;
	constexpr float MaxSideDistance = 5000.0f;
	constexpr float MinSideHeight = -500.0f;
	constexpr float MaxSideHeight = 1000.0f;
	constexpr float MinLookAhead = 0.0f;
	constexpr float MaxLookAhead = 1000.0f;
	constexpr float MinSideFieldOfView = 30.0f;
	constexpr float MaxSideFieldOfView = 120.0f;

	/** The SideScroll options' names, in the order the docs list them: Path, Distance, Height, LookAhead, FieldOfView, Bounds. */
	MANTLERUNTIME_API TConstArrayView<const TCHAR*> SideScrollOptionNames();
	/** Distance, Height, LookAhead or FieldOfView (case sensitive). */
	MANTLERUNTIME_API bool IsSideScrollNumberOption(const FString& Key);

	/** The flat length of Path, cm: the sum of its segments, in double like the Go and Python checkers. */
	MANTLERUNTIME_API double PathLength(const TArray<FVector2D>& Path);

	/** Sets a numeric SideScroll option. Same contract as ApplyNumber: false with OutError naming the option and its range. */
	MANTLERUNTIME_API bool ApplySideScrollNumber(FMantleSideScrollOptions& Options, const FString& Key, double Value, FString& OutError);
	/**
	 * Sets Path from Points (Z dropped): 2 to MaxPathPoints points, all finite, every segment at least MinSegmentLength long. False with
	 * OutError ("Path needs at least 2 points", "Path point 3 is not a finite number", "Path segment 2 is shorter than 50 cm").
	 */
	MANTLERUNTIME_API bool ApplySideScrollPath(FMantleSideScrollOptions& Options, const TArray<FVector>& Points, FString& OutError);
	/** Sets Bounds: both finite and Min below Max. Their range against the path is FinishSideScroll's, since the path may come later. */
	MANTLERUNTIME_API bool ApplySideScrollBounds(FMantleSideScrollOptions& Options, double Min, double Max, FString& OutError);
	/** The checks across options: Bounds needs a Path and sits within 0 to its length. */
	MANTLERUNTIME_API bool FinishSideScroll(const FMantleSideScrollOptions& Options, FString& OutError);
}
