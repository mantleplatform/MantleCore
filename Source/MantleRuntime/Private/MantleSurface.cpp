// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSurface.h"
#include "MantlePhysicsProp.h"
#include "MantleRuntime.h"
#include "MantleTerrainSurface.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

namespace MantleSurfacePrivate
{
	/** In EMantleSurface order. The Name column of DefaultEngine.ini's PhysicalSurfaces is these, capitalised. */
	const TCHAR* const GSurfaceNames[] = {
		TEXT("default"), TEXT("concrete"), TEXT("stone"), TEXT("brick"), TEXT("asphalt"), TEXT("wood"),
		TEXT("metal"), TEXT("glass"), TEXT("dirt"), TEXT("grass"), TEXT("sand"), TEXT("mud"), TEXT("snow"),
		TEXT("ice"), TEXT("water"), TEXT("rubber"), TEXT("plastic"), TEXT("fabric"), TEXT("flesh"),
		TEXT("gravel"), TEXT("leaf"),
	};
	static_assert(static_cast<int32>(UE_ARRAY_COUNT(GSurfaceNames)) == MantleSurfaceCount, "one name per EMantleSurface");

	/** The asset of each surface's own physical material, when no physics.material preset carries it. */
	const TCHAR* const GSurfaceAssetNames[] = {
		nullptr, TEXT("PM_MantleSurface_Concrete"), nullptr, TEXT("PM_MantleSurface_Brick"),
		TEXT("PM_MantleSurface_Asphalt"), nullptr, nullptr, TEXT("PM_MantleSurface_Glass"),
		TEXT("PM_MantleSurface_Dirt"), TEXT("PM_MantleSurface_Grass"), TEXT("PM_MantleSurface_Sand"),
		TEXT("PM_MantleSurface_Mud"), TEXT("PM_MantleSurface_Snow"), nullptr, TEXT("PM_MantleSurface_Water"),
		nullptr, TEXT("PM_MantleSurface_Plastic"), TEXT("PM_MantleSurface_Fabric"), TEXT("PM_MantleSurface_Flesh"),
		TEXT("PM_MantleSurface_Gravel"), TEXT("PM_MantleSurface_Leaf"),
	};
	static_assert(static_cast<int32>(UE_ARRAY_COUNT(GSurfaceAssetNames)) == MantleSurfaceCount, "one entry per EMantleSurface");

	/** The physics.material preset that carries a surface, if one does. */
	bool PresetForSurface(EMantleSurface Surface, EMantlePhysicsMaterial& OutPreset)
	{
		for (int32 Index = 0; Index < MantlePhysicsMaterialCount; ++Index)
		{
			const EMantlePhysicsMaterial Preset = static_cast<EMantlePhysicsMaterial>(Index);
			if (MantlePhysicsMaterialSurface(Preset) == Surface)
			{
				OutPreset = Preset;
				return true;
			}
		}
		return false;
	}
}

const TCHAR* MantleSurfaceToString(EMantleSurface Surface)
{
	const int32 Index = static_cast<int32>(Surface);
	return (Index >= 0 && Index < MantleSurfaceCount) ? MantleSurfacePrivate::GSurfaceNames[Index] : TEXT("default");
}

bool MantleParseSurface(const FString& In, EMantleSurface& Out)
{
	for (int32 Index = 0; Index < MantleSurfaceCount; ++Index)
	{
		if (In.Equals(MantleSurfacePrivate::GSurfaceNames[Index], ESearchCase::CaseSensitive))
		{
			Out = static_cast<EMantleSurface>(Index);
			return true;
		}
	}
	return false;
}

FString MantleSurfaceNameList()
{
	FString List;
	for (int32 Index = 0; Index < MantleSurfaceCount; ++Index)
	{
		List += Index > 0 ? TEXT(", ") : TEXT("");
		List += MantleSurfacePrivate::GSurfaceNames[Index];
	}
	return List;
}

EMantleSurface MantleSurfaceFromByte(uint8 SurfaceByte)
{
	return SurfaceByte < MantleSurfaceCount ? static_cast<EMantleSurface>(SurfaceByte) : EMantleSurface::Default;
}

EMantleSurface MantleSurfaceFromSurfaceType(int32 SurfaceTypeIndex)
{
	return (SurfaceTypeIndex >= 0 && SurfaceTypeIndex < MantleSurfaceCount)
		? static_cast<EMantleSurface>(SurfaceTypeIndex) : EMantleSurface::Default;
}

EMantleSurface MantleSurfaceFromPhysicalMaterial(const UPhysicalMaterial* Material)
{
	return Material ? MantleSurfaceFromSurfaceType(static_cast<int32>(Material->SurfaceType.GetValue())) : EMantleSurface::Default;
}

EMantleSurface MantleSurfaceFromHit(const FHitResult& Hit)
{
	const EMantleSurface FromMaterial = MantleSurfaceFromPhysicalMaterial(Hit.PhysMaterial.Get());
	if (FromMaterial != EMantleSurface::Default)
	{
		return FromMaterial;
	}
	// The hit component's Phys Material Override (the builder puts a tagged mesh's surface
	// there). The engine gives it to simple collision only; a mesh whose collision is its triangles
	// (complex as simple, as runtime-built meshes are) reports its materials' physical material in
	// the hit instead, so the override is read from the component here.
	if (const UPrimitiveComponent* Component = Hit.GetComponent())
	{
		const EMantleSurface FromOverride = MantleSurfaceFromPhysicalMaterial(Component->BodyInstance.GetPhysMaterialOverride());
		if (FromOverride != EMantleSurface::Default)
		{
			return FromOverride;
		}
	}
	const AActor* Actor = Hit.GetActor();
	// A built terrain's chunks carry no surface of their own: the weightmap says which layer is there.
	if (const UMantleTerrainSurfaceComponent* Terrain = Actor ? Actor->FindComponentByClass<UMantleTerrainSurfaceComponent>() : nullptr)
	{
		return Terrain->SurfaceAt(Hit.ImpactPoint);
	}
	return Cast<ACharacter>(Actor) ? EMantleSurface::Flesh : EMantleSurface::Default;
}

EMantleSurface MantlePhysicsMaterialSurface(EMantlePhysicsMaterial Material)
{
	switch (Material)
	{
	case EMantlePhysicsMaterial::Rubber: return EMantleSurface::Rubber;
	case EMantlePhysicsMaterial::Ice:    return EMantleSurface::Ice;
	case EMantlePhysicsMaterial::Metal:  return EMantleSurface::Metal;
	case EMantlePhysicsMaterial::Wood:   return EMantleSurface::Wood;
	case EMantlePhysicsMaterial::Stone:  return EMantleSurface::Stone;
	case EMantlePhysicsMaterial::Default:
	default:                             return EMantleSurface::Default;
	}
}

FString MantleSurfaceMaterialAssetPath(EMantleSurface Surface)
{
	EMantlePhysicsMaterial Preset;
	if (MantleSurfacePrivate::PresetForSurface(Surface, Preset))
	{
		return MantlePhysicsMaterialAssetPath(Preset);
	}
	const int32 Index = static_cast<int32>(Surface);
	const TCHAR* Name = (Index >= 0 && Index < MantleSurfaceCount) ? MantleSurfacePrivate::GSurfaceAssetNames[Index] : nullptr;
	return Name ? FString::Printf(TEXT("/Game/Mantle/Physics/Surfaces/%s.%s"), Name, Name) : MantlePhysicsMaterialAssetPath(EMantlePhysicsMaterial::Default);
}

UPhysicalMaterial* MantleLoadSurfaceMaterial(EMantleSurface Surface)
{
	// As MantleLoadPhysicsMaterial: a missing asset stays missing for the process (a cooked build
	// can't gain one), so it is looked for, and logged, once rather than for every mesh using it.
	static bool bMissing[MantleSurfaceCount] = {};
	const int32 Index = static_cast<int32>(MantleSurfaceFromByte(static_cast<uint8>(Surface)));
	if (bMissing[Index])
	{
		return nullptr;
	}
	const FString Path = MantleSurfaceMaterialAssetPath(Surface);
	UPhysicalMaterial* Loaded = LoadObject<UPhysicalMaterial>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Loaded)
	{
		bMissing[Index] = true;
		UE_LOG(LogMantle, Warning,
			TEXT("MANTLE_SURFACE_MATERIAL_MISSING surface=%s path=%s: this build has no such physical material, so geometry with it keeps its own surface. ")
			TEXT("Create it with tools/physics/Create-MantlePhysicalMaterials.py and cook /Game/Mantle/Physics."),
			MantleSurfaceToString(Surface), *Path);
	}
	return Loaded;
}
