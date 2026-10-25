// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEditorStyle.h"

#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr<FSlateStyleSet> FMantleEditorStyle::StyleSet;

void FMantleEditorStyle::Initialize()
{
	if (StyleSet.IsValid())
	{
		return;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	if (!Plugin.IsValid())
	{
		return;
	}

	StyleSet = MakeShared<FSlateStyleSet>(GetStyleSetName());
	StyleSet->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));

	const FVector2f Icon16(16.0f, 16.0f);
	const FVector2f Icon20(20.0f, 20.0f);
	const FString TestLocallyFile = StyleSet->RootToContentDir(FString(TEXT("Icons/MantleTestLocally")), TEXT(".svg"));
	const FString LogoFile = StyleSet->RootToContentDir(FString(TEXT("Icons/MantleLogo")), TEXT(".svg"));

	StyleSet->Set("Mantle.TestLocally", new FSlateVectorImageBrush(TestLocallyFile, Icon20));
	StyleSet->Set("Mantle.TestLocally.Small", new FSlateVectorImageBrush(TestLocallyFile, Icon16));
	StyleSet->Set("Mantle.Logo", new FSlateVectorImageBrush(LogoFile, Icon20));
	StyleSet->Set("Mantle.Logo.Small", new FSlateVectorImageBrush(LogoFile, Icon16));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}

void FMantleEditorStyle::Shutdown()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
}

FName FMantleEditorStyle::GetStyleSetName()
{
	return FName(TEXT("MantleEditorStyle"));
}

FSlateIcon FMantleEditorStyle::TestLocallyIcon()
{
	return FSlateIcon(GetStyleSetName(), "Mantle.TestLocally", "Mantle.TestLocally.Small");
}

FSlateIcon FMantleEditorStyle::TestLocallyMenuIcon()
{
	return FSlateIcon(GetStyleSetName(), "Mantle.TestLocally.Small", "Mantle.TestLocally.Small");
}

FSlateIcon FMantleEditorStyle::LogoIcon()
{
	return FSlateIcon(GetStyleSetName(), "Mantle.Logo", "Mantle.Logo.Small");
}
