// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UActorComponent;
class UStaticMeshComponent;

/**
 * Water plugin bodies (WaterBodyCustom, Lake, River, Ocean components), recognised by class name (GhzGarage/mantle#818, #997). MantleEditor
 * does not link the Water plugin. The exporter leaves a body out with a warning until the place format carries it (FMantleWaterBodyExport
 * reads it); an ordinary static mesh with a water material still publishes with Mantle's water.
 */
class MANTLEEDITOR_API FMantleWaterBody
{
public:
	/** Whether Component is a Water plugin body component (UWaterBodyComponent or a child). */
	static bool IsWaterBody(const UActorComponent* Component);

	/**
	 * Whether Mesh is the static mesh component a Water plugin body on the same actor draws itself with (a Custom body's MeshComp, or
	 * any body's water mesh override): the exporter leaves it out with its body.
	 */
	static bool IsBodyMeshComponent(const UStaticMeshComponent* Mesh);

	/** "custom", "lake", "river", "ocean", or "water" for any other body. */
	static FString Kind(const UActorComponent* Body);
};
