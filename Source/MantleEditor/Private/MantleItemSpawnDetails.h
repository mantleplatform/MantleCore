// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;

/**
 * The Details panel of a Mantle Item Spawn (#235): Model Id, the place asset id the runtime actor holds, is replaced by a Static Mesh
 * picker. The picked mesh is stored as its object path in the same text (FMantleItemSpawnModel), and the exporter turns it into the
 * asset id and uploads the mesh with the place. A spawn loaded from a place file keeps its asset id, shown under the picker.
 */
class FMantleItemSpawnDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** Registers and unregisters the customization with the PropertyEditor module (module startup and shutdown). */
	static void Register();
	static void Unregister();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
