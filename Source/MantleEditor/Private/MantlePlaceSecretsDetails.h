// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;

/**
 * Place Settings > Secrets (GhzGarage/mantle#1011): the names of the place's secrets with "set <time>" and a masked value
 * (the plugin never has one), Add or Replace through a write-only value field, Delete after a confirm, n / max, and the
 * secrets.local.json button for local tests. The value is held in the panel's own state only: never a property of the actor
 * (so never in undo, the place file or a .uasset), never logged, cleared after a successful save.
 */
class FMantlePlaceSecretsDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** Registers and unregisters the customization with the PropertyEditor module (module startup and shutdown). */
	static void Register();
	static void Unregister();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
