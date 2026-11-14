// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Whether this editor can build Nanite data at all (it needs a graphics device's shader platform; the first call builds a small mesh
 * to find out). Tests that read built Nanite data skip when it cannot, and a test that expects a file from a conversion fails when
 * the editor can build one and the conversion made none. Defined in MantleMeshNaniteBakeTest.cpp.
 */
bool MantleNaniteTestEditorBuildsNanite();

#endif
