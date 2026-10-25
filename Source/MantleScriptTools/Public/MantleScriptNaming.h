// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Converts canonical PascalCase to each language's convention. */
struct MANTLESCRIPTTOOLS_API FMantleNaming
{
	static FString ToCamel(const FString& Pascal);  // FindByTag -> findByTag
	static FString ToSnake(const FString& Pascal);  // FindByTag -> find_by_tag
};
