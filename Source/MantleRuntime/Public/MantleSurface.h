// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UPhysicalMaterial;
struct FHitResult;

// Declared, not included: MantlePlaceTypes.h includes this header, since a place's meshes and
// terrain layers carry an EMantleSurface (#596 step 1b). Defined there.
enum class EMantlePhysicsMaterial : uint8;

/**
 * What a surface is made of (#596): what an effect (bullet impact, decal, wheel dust, footstep)
 * should look like where something touched it.
 *
 * Each value is the project surface type of the same index: Default is SurfaceType_Default and
 * Concrete is SurfaceType1, and so on. client/Config/DefaultEngine.ini names them in this order
 * under [/Script/Engine.PhysicsSettings] PhysicalSurfaces. A UPhysicalMaterial carries one as its
 * SurfaceType, so a hit's physical material says which surface it struck. The value travels as
 * one byte (AMantleWeapon::MulticastShot) and names are the wire's too, so append, never reorder.
 */
enum class EMantleSurface : uint8
{
	Default,
	Concrete,
	Stone,
	Brick,
	Asphalt,
	Wood,
	Metal,
	Glass,
	Dirt,
	Grass,
	Sand,
	Mud,
	Snow,
	Ice,
	Water,
	Rubber,
	Plastic,
	Fabric,
	Flesh,
	Gravel,
	Leaf
};

/** How many EMantleSurface values there are. */
inline constexpr int32 MantleSurfaceCount = 21;

/** The surface's name, lower case ("concrete"), as a place file or script would write it. */
MANTLERUNTIME_API const TCHAR* MantleSurfaceToString(EMantleSurface Surface);

/** Parses a MantleSurfaceToString name, case-exact. False (Out untouched) for anything else. */
MANTLERUNTIME_API bool MantleParseSurface(const FString& In, EMantleSurface& Out);

/** Every surface's name in order, comma separated, for an error message: "default, concrete, ..., flesh". */
MANTLERUNTIME_API FString MantleSurfaceNameList();

/** The surface a byte off the wire names; Default for a byte past the last value. */
MANTLERUNTIME_API EMantleSurface MantleSurfaceFromByte(uint8 SurfaceByte);

/** The surface of an engine surface type index (EPhysicalSurface); Default past the last value. */
MANTLERUNTIME_API EMantleSurface MantleSurfaceFromSurfaceType(int32 SurfaceTypeIndex);

/** The surface a physical material carries; Default for none. */
MANTLERUNTIME_API EMantleSurface MantleSurfaceFromPhysicalMaterial(const UPhysicalMaterial* Material);

/**
 * What a hit struck. The hit's physical material decides when it carries a surface, so a trace
 * wants bReturnPhysicalMaterial. Without one, the hit component's Phys Material Override decides
 * (the engine applies it to simple collision only, so a complex-collision hit never reports it
 * itself), then a built terrain answers with the surface of the
 * layer that weighs most at the hit point (UMantleTerrainSurfaceComponent), a character is Flesh,
 * and anything else is Default.
 */
MANTLERUNTIME_API EMantleSurface MantleSurfaceFromHit(const FHitResult& Hit);

/** The surface a physics.material preset carries (#528): default is Default, metal is Metal. */
MANTLERUNTIME_API EMantleSurface MantlePhysicsMaterialSurface(EMantlePhysicsMaterial Material);

/**
 * The shipped physical material for a surface. A surface a physics.material preset already
 * carries uses that preset's asset (/Game/Mantle/Physics/PM_MantleMetal); every other surface has
 * its own under /Game/Mantle/Physics/Surfaces (PM_MantleSurface_Concrete). Both are written by
 * tools/physics/Create-MantlePhysicalMaterials.py.
 */
MANTLERUNTIME_API FString MantleSurfaceMaterialAssetPath(EMantleSurface Surface);

/**
 * The shipped physical material for a surface (MantleSurfaceMaterialAssetPath), loaded by path as
 * MantleLoadPhysicsMaterial loads a preset. Null when this build lacks it, which is looked for and
 * logged (MANTLE_SURFACE_MATERIAL_MISSING) once per surface per process; geometry then keeps its
 * own physical material. Game thread.
 */
MANTLERUNTIME_API UPhysicalMaterial* MantleLoadSurfaceMaterial(EMantleSurface Surface);
