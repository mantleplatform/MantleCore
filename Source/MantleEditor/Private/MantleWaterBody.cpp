// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWaterBody.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWaterBodyPrivate
{
	bool DerivesFrom(const UClass* Class, const TCHAR* Name)
	{
		for (; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == FName(Name))
			{
				return true;
			}
		}
		return false;
	}

	/** An object property of the body by name (the Water module's members are not linked, so they are read by reflection). */
	UObject* ReadObject(const UObject* Body, const TCHAR* Property)
	{
		const FObjectProperty* Found = CastField<FObjectProperty>(Body->GetClass()->FindPropertyByName(FName(Property)));
		return Found ? Found->GetObjectPropertyValue_InContainer(Body) : nullptr;
	}

	FString KindOf(const UClass* Class)
	{
		if (DerivesFrom(Class, TEXT("WaterBodyCustomComponent")))
		{
			return TEXT("custom");
		}
		if (DerivesFrom(Class, TEXT("WaterBodyLakeComponent")))
		{
			return TEXT("lake");
		}
		if (DerivesFrom(Class, TEXT("WaterBodyRiverComponent")))
		{
			return TEXT("river");
		}
		if (DerivesFrom(Class, TEXT("WaterBodyOceanComponent")))
		{
			return TEXT("ocean");
		}
		return TEXT("water");
	}
}

bool FMantleWaterBody::IsWaterBody(const UActorComponent* Component)
{
	return Component && MantleWaterBodyPrivate::DerivesFrom(Component->GetClass(), TEXT("WaterBodyComponent"));
}

bool FMantleWaterBody::IsBodyMeshComponent(const UStaticMeshComponent* Mesh)
{
	using namespace MantleWaterBodyPrivate;
	const AActor* Owner = Mesh ? Mesh->GetOwner() : nullptr;
	if (!Owner)
	{
		return false;
	}
	// Whatever the plugin hangs under a body (a lake's surface mesh, a river's pieces) and its own water mesh component classes.
	if (DerivesFrom(Mesh->GetClass(), TEXT("WaterBodyMeshComponent")))
	{
		return true;
	}
	for (const USceneComponent* Parent = Mesh->GetAttachParent(); Parent; Parent = Parent->GetAttachParent())
	{
		if (IsWaterBody(Parent))
		{
			return true;
		}
	}
	for (const UActorComponent* Component : Owner->GetComponents())
	{
		if (!IsWaterBody(Component))
		{
			continue;
		}
		// A Custom body's own mesh component (MeshComp, attached to it in the plugin but found by name too), else the mesh any body's override names.
		if (ReadObject(Component, TEXT("MeshComp")) == Mesh)
		{
			return true;
		}
		const UStaticMesh* Override = Cast<UStaticMesh>(ReadObject(Component, TEXT("WaterMeshOverride")));
		if (Override && Override == Mesh->GetStaticMesh())
		{
			return true;
		}
	}
	return false;
}

FString FMantleWaterBody::Kind(const UActorComponent* Body)
{
	return Body ? MantleWaterBodyPrivate::KindOf(Body->GetClass()) : FString();
}
