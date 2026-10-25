// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/**
 * The Mantle editor's own icons (#563 stage 3): the Test Locally button and the Mantle toolbar combo.
 * SVGs under MantleCore/Resources/Icons, drawn in white so the toolbar tints them like its own icons.
 * Registered by the module at startup, unregistered at shutdown.
 */
class FMantleEditorStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static FName GetStyleSetName();

	/** The Test Locally glyph, 20 px (the toolbar) and 16 px (menus). */
	static FSlateIcon TestLocallyIcon();
	static FSlateIcon TestLocallyMenuIcon();

	/** The Mantle mark for the toolbar's Mantle combo. */
	static FSlateIcon LogoIcon();

private:
	static TSharedPtr<FSlateStyleSet> StyleSet;
};
