// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "MantleOverlayGuideLibrary.generated.h"

/** Headless access to the overlay painting guide (editor Python). */
UCLASS()
class MANTLEEDITOR_API UMantleOverlayGuideLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Writes the painting guide PNG for the body (bFace false) or the face to Path. Returns an empty string on success, else why not. */
	UFUNCTION(BlueprintCallable, Category="Mantle|Overlay")
	static FString SaveOverlayGuide(bool bFace, FString Path);

	/** Rebuilds both guides from MetaHuman Creator's template meshes into the plugin's Resources/OverlayGuides. Returns an empty string on success. */
	UFUNCTION(BlueprintCallable, Category="Mantle|Overlay")
	static FString RegenerateShippedOverlayGuides();
};
