// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The gated features this client build reads (#628). Each is one of the backend's release flags
 * (backend/internal/compatibility/compatibility.go) under a stable lowercase name: content a place or an
 * asset can carry that an older released client cannot read. The preview client lists them in its
 * -MantlePreviewCapabilities answer ("features") so Test Locally can export and cook for it: the cook tool lets
 * their gated keys past the release check, and the editor leaves out what a target does not read. An older
 * client's answer has no such key, which means it reads none.
 *
 * A build reads every feature it names, so the list is a constant, not a probe. A feature stays in it once its
 * flag is released. A test (compatibility.TestClientFeaturesHeaderMatchesTheRegistry) holds these names to the
 * backend's, one for one: add a flag there and its name here, in the constants and in All().
 */
namespace MantleClientFeatures
{
	inline const TCHAR* const InstancedMesh = TEXT("instancedMesh");
	inline const TCHAR* const CrowdSpawner = TEXT("crowdSpawner");
	inline const TCHAR* const CrowdPointsOfInterest = TEXT("crowdPointsOfInterest");
	inline const TCHAR* const SkeletalMesh = TEXT("skeletalMesh");
	inline const TCHAR* const RectLight = TEXT("rectLight");
	inline const TCHAR* const Decal = TEXT("decal");
	inline const TCHAR* const LightShape = TEXT("lightShape");
	inline const TCHAR* const Terrain = TEXT("terrain");
	inline const TCHAR* const TerrainGrass = TEXT("terrainGrass");
	inline const TCHAR* const Vehicle = TEXT("vehicle");
	inline const TCHAR* const VehicleLinkage = TEXT("vehicleLinkage");
	inline const TCHAR* const VehicleParts = TEXT("vehicleParts");
	inline const TCHAR* const VehicleCarInteraction = TEXT("vehicleCarInteraction");
	inline const TCHAR* const VehicleContactPoints = TEXT("vehicleContactPoints");
	/** A vehicle seat's hidden key (#728): the seated occupant is not drawn. */
	inline const TCHAR* const VehicleHiddenSeat = TEXT("vehicleHiddenSeat");
	/** A vehicle document's movementType plane and helicopter with their flight and helicopter blocks and the flier's camera boom (#821). */
	inline const TCHAR* const VehicleFliers = TEXT("vehicleFliers");
	/** A vehicle document's movementType hover and its hover block (#821). */
	inline const TCHAR* const VehicleHover = TEXT("vehicleHover");
	/** A vehicle seat's kind (motorcycle, pilot), exit distance and a motorcycle driver's rider block (#821). */
	inline const TCHAR* const VehicleSeatKind = TEXT("vehicleSeatKind");
	/** An authored per-seat yaw and pitch arc for held weapons (#615). */
	inline const TCHAR* const VehicleWeaponArc = TEXT("vehicleWeaponArc");
	inline const TCHAR* const Effect = TEXT("effect");
	/** The sprites archetype of an Effect (#628): NS_MantleSprites and its materials, and the keys that tune it. */
	inline const TCHAR* const EffectSprites = TEXT("effectSprites");
	inline const TCHAR* const Constraint = TEXT("constraint");
	inline const TCHAR* const Scatter = TEXT("scatter");
	/** ReflectionProbe entities (#628): a box or sphere captured at run time, and the project's r.ReflectionCapture.Runtime=1 they need. */
	inline const TCHAR* const ReflectionProbes = TEXT("reflectionProbes");
	inline const TCHAR* const Storage = TEXT("storage");
	inline const TCHAR* const UIDocuments = TEXT("uiDocuments");
	inline const TCHAR* const EnvironmentDefaults = TEXT("environmentDefaults");
	/** settings.environment.skyLight and settings.environment.sun (#722): the place's sky light scale and a sun that never reaches it. */
	inline const TCHAR* const EnvironmentLighting = TEXT("environmentLighting");
	inline const TCHAR* const AvatarHeight = TEXT("avatarHeight");
	inline const TCHAR* const SoundEmitterAutoplay = TEXT("soundEmitterAutoplay");
	inline const TCHAR* const StaticMeshPhysics = TEXT("staticMeshPhysics");
	inline const TCHAR* const PhysicsMaterial = TEXT("physicsMaterial");
	inline const TCHAR* const Surface = TEXT("surface");
	/** A StaticMesh's or an InstancedMesh's drawDistance (#630 stage 2): culled, faded and shadowless by distance. */
	inline const TCHAR* const DrawDistance = TEXT("drawDistance");
	/** Shading carPaint and its finish (#487). */
	inline const TCHAR* const CarPaint = TEXT("carPaint");
	/** #471's material keys, #533's and #628's foliage, and the extended master with their permutations. */
	inline const TCHAR* const MaterialFidelity = TEXT("materialFidelity");
	/** alphaMode "additive" in a material document (#628): its two Additive permutations of M_MantlePlace_Extended. */
	inline const TCHAR* const MaterialAdditive = TEXT("materialAdditive");
	/** A preview manifest's "weapons" section (#729): a weapon document, its model, and equipping it on spawn. */
	inline const TCHAR* const WeaponDocument = TEXT("weaponDocument");
	/** Text entities (GhzGarage/mantle#724): world text drawn in Mantle's built-in font. */
	inline const TCHAR* const WorldText = TEXT("worldText");

	/** Every feature this build reads, in the order above. */
	inline TArray<FString> All()
	{
		return {
			InstancedMesh, CrowdSpawner, CrowdPointsOfInterest, SkeletalMesh, RectLight, Decal, LightShape, Terrain, TerrainGrass,
			Vehicle, VehicleLinkage, VehicleParts, VehicleCarInteraction, VehicleContactPoints, VehicleHiddenSeat, VehicleFliers, VehicleHover, VehicleSeatKind, VehicleWeaponArc, Effect, EffectSprites, Constraint, Scatter, ReflectionProbes, Storage,
			UIDocuments, EnvironmentDefaults, EnvironmentLighting, AvatarHeight, SoundEmitterAutoplay, StaticMeshPhysics, PhysicsMaterial, Surface, DrawDistance, CarPaint,
			MaterialFidelity, MaterialAdditive, WeaponDocument, WorldText
		};
	}
}
