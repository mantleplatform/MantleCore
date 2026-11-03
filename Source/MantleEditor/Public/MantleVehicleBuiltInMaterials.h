// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The built-in vehicle materials a vehicle may name where it names a material (mantle#239: ast_mat_rubber, ast_mat_glass, ast_mat_chrome, ast_mat_light_lens): Mantle's own
 * tire rubber, windows, chrome and light lenses, shipped in the client, so nothing uploads and a creator needs no material of their own for them. The builder's Seats and
 * paint step suggests one for a section whose name says what it is; the creator turns each suggestion off. Nothing is ever forced.
 */
struct MANTLEEDITOR_API FMantleVehicleBuiltInMaterials
{
	static const TCHAR* Rubber() { return TEXT("ast_mat_rubber"); }
	static const TCHAR* Glass() { return TEXT("ast_mat_glass"); }
	static const TCHAR* Chrome() { return TEXT("ast_mat_chrome"); }
	static const TCHAR* LightLens() { return TEXT("ast_mat_light_lens"); }

	/** Every built-in vehicle material id. */
	static TArray<FString> All();

	/** Whether Id is one of them. */
	static bool IsBuiltIn(const FString& Id);

	/**
	 * The built-in material a section's name suggests, or empty: glass for a name with glass, window, windscreen or windshield; rubber for tire or tyre (not a rim, hub, alloy or
	 * brake); chrome for chrome; a light lens for lens. A name with a lens in it is a light lens before it is glass (a headlight lens or lens glass). Any case; a separator
	 * between words is not needed ("FrontWindowGlass" reads as glass). A wiper, a frame, a seal or a trim is not the glass it sits next to.
	 */
	static FString Suggest(const FString& SectionName);

	/** The material's name for a creator: "Tire rubber", "Glass", "Chrome", "Light lens". */
	static FText Label(const FString& Id);
};
