// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class FMantleCoverage;
class UTextRenderComponent;
struct FMantleValidationResult;

/**
 * What the exporter does with a Text Render component (GhzGarage/mantle#724): it publishes as a Text entity, on a
 * placed Text Render Actor and on any other actor, a Blueprint's or a Group's part. Mantle draws it in its built-in
 * font and material, so a custom font or text material is approximated. Needs the worldText client feature: on a
 * target without it the text is left out, and the coverage report says the target needs a newer Mantle client.
 *
 * The shared exporter, validator and survey files only call this.
 */
class MANTLEEDITOR_API FMantleTextExport
{
public:
	/** The text component of a placed Text Render Actor (the engine's own class, exactly), or null for any other actor. */
	static const UTextRenderComponent* ActorComponent(const AActor* Actor);

	/** Whether Actor is a placed Text Render Actor, which publishes as a Text entity. */
	static bool IsTextActor(const AActor* Actor);

	/**
	 * Whether Component is text a player would read: not editor-only or transient, shown in game, and with something to
	 * read. Anything else is nothing to publish.
	 */
	static bool IsShownText(const UTextRenderComponent* Component);

	/** The component's text as a place holds it: UTF-8 with "\n" for a line break (the engine's "<br>" and "\r\n" included). */
	static FString PublishedText(const UTextRenderComponent* Component);

	/**
	 * Reads Component into Out, inside the limits a place holds. Scale is the scale Component is published at: the entity's
	 * transform is written at scale 1, so its Y scale (along the line) multiplies xScale and its Z scale (up the glyph)
	 * multiplies yScale, as the engine's component scale does. A value outside the range a place holds is clamped, and
	 * said in OutApproximations, as is everything Mantle draws differently: a custom font or text material, Quad Top
	 * alignment, spacing adjustments, a mirrored scale. Text longer than FMantlePlaceLimits::MaxTextLength is not cut: see Refusal.
	 */
	static void Read(const UTextRenderComponent* Component, const FVector& Scale, FMantleTextProps& Out, TArray<FString>& OutApproximations);

	/** Why Props cannot publish (the text is over FMantlePlaceLimits::MaxTextLength code points), or empty. */
	static FString Refusal(const FMantleTextProps& Props);

	/**
	 * Writes Entity's properties from Component, folding Entity.Transform's scale into xScale and yScale and
	 * leaving the transform at scale 1 (FMantlePlaceExporter's case for a Text actor, and for a prefab's text part).
	 */
	static void Extract(const UTextRenderComponent* Component, FMantleEntity& Entity);

	/**
	 * Export's pass 1 for Component, on actor Label: counts it in InOutTexts and refuses (Errors, at Path) text that is too
	 * long or past FMantlePlaceLimits::MaxTextsPerPlace; reports the coverage line (Kept, or Approximated naming what Mantle
	 * draws differently) when Coverage is given. A refused text is still counted.
	 */
	static void Admit(const UTextRenderComponent* Component, const FString& Label, const FString& Path, const FVector& Scale,
		int32& InOutTexts, FMantleCoverage* Coverage, TArray<FMantleParseError>& Errors);

	/**
	 * Check this place's note for the text on Actor: a Text actor's, and a Group's text parts. An error for text over
	 * FMantlePlaceLimits::MaxTextLength and for the text past MaxTextsPerPlace; a warning for what Mantle draws differently.
	 * Does nothing for a target without worldText (the text is left out and said so elsewhere).
	 */
	static void Check(const AActor* Actor, EMantleEntityType Type, int32& InOutTexts, FMantleValidationResult& Result);
};
