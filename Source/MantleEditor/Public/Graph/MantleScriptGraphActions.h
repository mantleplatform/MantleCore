// Copyright Joshua Eger. All Rights Reserved.
// Content Browser presentation (icon colour, category, double-click) for UMantleScriptGraph.
#pragma once

#include "CoreMinimal.h"
#include "AssetTypeCategories.h"
#include "AssetTypeActions_Base.h"

class MANTLEEDITOR_API FMantleScriptGraphActions : public FAssetTypeActions_Base
{
public:
	explicit FMantleScriptGraphActions(EAssetTypeCategories::Type InAssetCategory);

	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor) override;

private:
	EAssetTypeCategories::Type AssetCategory;
};
