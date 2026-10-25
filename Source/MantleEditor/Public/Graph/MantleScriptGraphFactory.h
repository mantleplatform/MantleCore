// Copyright Joshua Eger. All Rights Reserved.
// "Mantle > Script Graph" in the Content Browser's right-click New menu.
#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MantleScriptGraphFactory.generated.h"

UCLASS()
class MANTLEEDITOR_API UMantleScriptGraphFactory : public UFactory
{
	GENERATED_BODY()

public:
	UMantleScriptGraphFactory();

	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;
};
