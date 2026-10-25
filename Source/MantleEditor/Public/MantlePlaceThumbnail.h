// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"

class UWorld;

/** Where a place's thumbnail is taken from (#786), in the level's world coordinates. */
struct MANTLEEDITOR_API FMantlePlaceThumbnailView
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float FOV = 60.f;

	/** True when Mantle framed it (no view chosen on Mantle Place Settings). */
	bool bAutomatic = true;
};

/**
 * A published place's catalogue picture (#786): a 16:9 capture of the level itself, from the view chosen on Mantle Place
 * Settings (Thumbnail) or, when none is, from above and behind the first Player Start looking toward the middle of what
 * the level draws. Publish uploads it as a colour texture and the exporter names it in metadata.thumbnail; the
 * server holds the place out of discovery until the picture passes moderation.
 */
class MANTLEEDITOR_API FMantlePlaceThumbnail
{
public:
	/** The picture's size: 16:9, the place cards' art, inside the 2048 texture limit. */
	static constexpr int32 Width = 1280;
	static constexpr int32 Height = 720;

	/** The automatic view's horizontal field of view, in degrees. */
	static constexpr float AutoFOV = 60.f;

	/** How far above the ground a Player Start's view is lifted: a standing player's eyes. */
	static constexpr float EyeHeightCm = 170.f;

	/**
	 * The automatic view from what the level draws (Bounds; invalid when it draws nothing) and its first Player Start
	 * (Spawn; null when it has none). Pure and deterministic: the same level frames the same picture every publish.
	 */
	static FMantlePlaceThumbnailView AutoView(const FBox& Bounds, const FTransform* Spawn);

	/** AutoView for World: its drawn bounds and its first Player Start by name. */
	static FMantlePlaceThumbnailView AutoView(UWorld* World);

	/** The view chosen on World's Mantle Place Settings, or AutoView when none is. */
	static FMantlePlaceThumbnailView ViewFor(UWorld* World);

	/** What World draws in game, for framing: visible primitives' bounds, leaving out sky-sized ones. Invalid when nothing. */
	static FBox DrawnBounds(UWorld* World);

	/** False, saying why, when this editor cannot render a picture (-NullRHI, a commandlet). */
	static bool CanCapture(FString* OutWhyNot = nullptr);

	/** Renders World from View: Width x Height opaque pixels, top row first. */
	static bool Capture(UWorld* World, const FMantlePlaceThumbnailView& View, TArray<FColor>& OutPixels, FString& OutError);

	/** The thumbnail choice on World's Mantle Place Settings (#124); Automatic when it has none. */
	static FMantleThumbnailChoice ChoiceFor(UWorld* World);

	/** The shared resolver's render for a place: the level from its automatic view or the chosen camera (Capture). */
	static FMantleThumbnailResolver::FRender RenderFor(UWorld* World);

	/**
	 * The place's final picture (#124): Width x Height, from the settings' choice. A custom image needs no renderer; Automatic and Choose the angle do
	 * (CanCapture).
	 */
	static FMantleThumbnailResult Resolve(UWorld* World);

	/** The shared resolver's target for a place: Width x Height. */
	static FMantleThumbnailTarget Target() { return { Width, Height }; }
};
