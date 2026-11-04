// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMantleVehicleSource;
class UMaterialInterface;
class UStaticMesh;
struct FMantleVehicleSourcePart;

/**
 * The built-in vehicle materials a vehicle may name where it names a material (mantle#239: ast_mat_rubber, ast_mat_glass, ast_mat_chrome, ast_mat_light_lens, ast_mat_carbon): Mantle's own
 * tire rubber, windows, chrome, carbon fiber and light lenses, shipped in the client and shared by every car, so nothing uploads and creators do not pick them. The converter detects
 * the sections that are glass, tires, chrome, carbon fiber or lamps from what the model says (section and material names, translucency, the lights the builder found) and writes the
 * built-in id for them, as it gives paint slots car paint; every other section keeps the creator's own material. Nothing here is a creator setting.
 */
struct MANTLEEDITOR_API FMantleVehicleBuiltInMaterials
{
	static const TCHAR* Rubber() { return TEXT("ast_mat_rubber"); }
	static const TCHAR* Glass() { return TEXT("ast_mat_glass"); }
	static const TCHAR* Chrome() { return TEXT("ast_mat_chrome"); }
	static const TCHAR* LightLens() { return TEXT("ast_mat_light_lens"); }
	static const TCHAR* Carbon() { return TEXT("ast_mat_carbon"); }

	/** Every built-in vehicle material id. */
	static TArray<FString> All();

	/** Whether Id is one of them. */
	static bool IsBuiltIn(const FString& Id);

	/**
	 * The built-in material a section's name suggests, or empty: glass for a name with glass, window, windscreen or windshield; rubber for tire or tyre (not a rim, hub, alloy or
	 * brake); chrome for chrome; carbon fiber for carbon (carbonfiber, carbon_fiber; even a mirror or a glass-base strip made of it); a light lens for lens. A name with a lens in it is a light lens before it is glass (a headlight lens or lens glass). Any case; a separator
	 * between words is not needed ("FrontWindowGlass" reads as glass). A wiper, a frame, a seal or a trim is not the glass it sits next to.
	 */
	static FString Suggest(const FString& SectionName);

	/** The material's name for a creator: "Tire rubber", "Glass", "Chrome", "Light lens", "Carbon fiber". */
	static FText Label(const FString& Id);

	/**
	 * The built-in material a body section or part slot gets, or empty (it keeps the creator's material). Material is what it draws with. In order: a light section the
	 * builder found is a light lens; the name of the section, else of the material (Suggest); a lit Translucent material with one flat opacity under 0.95 is glass (a
	 * cutout or a decal, whose opacity comes from a texture, is not). A painted section or slot (the vehicle's color) is never replaced.
	 */
	static FString Detect(const UMaterialInterface* Material, const FString& SectionName, bool bLight);

	/** Detect for the body section SectionName (the builder's unique section name) of Source drawing Material: its light sections and paint are Source's. */
	static FString ForSection(const UMantleVehicleSource& Source, const FString& SectionName, const UMaterialInterface* Material);

	/** Detect for slot Slot of Part's mesh (its paint slots are Part's; a part is never a light). */
	static FString ForPartSlot(const FMantleVehicleSourcePart& Part, const UStaticMesh* Mesh, int32 Slot);

	/** What Source's sections and slots get, in plain words for the builder's notes ("Mantle's own glass for the windows: ..."); empty when nothing is replaced. */
	static FString Describe(const UMantleVehicleSource& Source);
};
