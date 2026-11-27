// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;

/**
 * The Details panel of a Mantle vehicle asset (#258): Tuning's Engine Sound is hidden on a vehicle that has no engine sound to pick (a pedalled vehicle,
 * a trailer, a boat, a plane, a helicopter or a hovercraft; an electric motor's is hidden by its own EditCondition). The struct's EditCondition cannot
 * see the asset's movement type or pedal, so the panel does it, and refreshes when one of them changes.
 */
class FMantleVehicleSourceDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** Registers and unregisters the customization with the PropertyEditor module (module startup and shutdown). */
	static void Register();
	static void Unregister();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
