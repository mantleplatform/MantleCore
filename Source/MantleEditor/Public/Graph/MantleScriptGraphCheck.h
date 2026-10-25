// Copyright Joshua Eger. All Rights Reserved.
// "Check this graph without opening the editor" -- the Scripts panel's Check All and
// Mantle.CheckScripts, for a UMantleScriptGraph row. Mirrors FMantleScriptGraphEditorToolkit::
// Compile's logic (build -> compile -> classify side) without needing a live toolkit instance, so
// Check All works exactly the same whether or not the graph happens to be open for editing.
#pragma once

#include "CoreMinimal.h"

class MANTLEEDITOR_API FMantleScriptGraphCheck
{
public:
	/** Loads the UMantleScriptGraph at PackageName, builds and compiles it. True and an empty
	 *  OutMessage on success (OutMessage still gets a short "N line(s)" note); false with
	 *  OutMessage set to the compiler's (or the folder classifier's) error otherwise. False with a
	 *  message naming the problem if the asset itself can't be loaded. */
	static bool CheckAsset(const FString& PackageName, FString& OutMessage);
};
