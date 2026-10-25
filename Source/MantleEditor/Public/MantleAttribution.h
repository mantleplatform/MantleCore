// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPath.h"
#include "MantleAttribution.generated.h"

/**
 * One Credits rule (#398): every uploaded asset under Folder (a content folder, such as the one a Fab
 * pack was added to) is credited with Attribution on the world page of any place that uses it.
 */
USTRUCT()
struct MANTLEEDITOR_API FMantleAttributionRule
{
	GENERATED_BODY()

	/** A content folder: /Game/Fab/Crates covers everything under it. */
	UPROPERTY(EditAnywhere, Category = "Credits", meta = (ContentDir, LongPackageName))
	FDirectoryPath Folder;

	/**
	 * What players are shown, as the licence asks for it: for Creative Commons BY, the title, the
	 * author, the source and the licence, e.g. "Crates" by Ann (fab.com/listings/...), CC BY 4.0.
	 * Plain text, one line, at most 500 characters.
	 */
	UPROPERTY(EditAnywhere, Category = "Credits", meta = (MultiLine = true))
	FString Attribution;
};

/**
 * Credits for uploaded place assets (#398), shared with the project (DefaultEditor.ini), since the
 * licence belongs to the content, not to one creator's editor. The least friction for a Fab pack of
 * hundreds of assets is one rule for the folder it was added to; a single asset can override its
 * folder with the "MantleAttribution" metadata tag on its package (for example
 * unreal.EditorAssetLibrary.set_metadata_tag(asset, "MantleAttribution", "...")), and an empty tag
 * value means none.
 *
 * Mantle cannot tell which licence content is under and never guesses: nothing is credited unless
 * a rule or a tag says so. Standard-licence Fab content needs no credit.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Mantle Credits"))
class MANTLEEDITOR_API UMantleAttributionSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** The deepest folder that contains an asset decides its attribution. */
	UPROPERTY(config, EditAnywhere, Category = "Credits", meta = (TitleProperty = "Attribution"))
	TArray<FMantleAttributionRule> Rules;
};

/** The rules for attribution text and which asset gets which (#398), kept apart from the editor so tests reach them. */
class MANTLEEDITOR_API FMantleAttribution
{
public:
	/** The API's limit (backend assets.MaxAttributionRunes), in characters after Normalize. */
	static constexpr int32 MaxCharacters = 500;

	/** The package metadata tag that overrides an asset's folder rule. */
	static const TCHAR* MetadataTag;

	/**
	 * The backend's normalization (assets.NormalizeAttribution), so what Check shows is what players
	 * see: control and invisible formatting characters dropped (the zero-width joiner kept), every
	 * whitespace run one space, trimmed.
	 */
	static FString Normalize(const FString& Text);

	/** Normalize's result fits the API's limit. */
	static bool IsWithinLimit(const FString& Text);

	/**
	 * The attribution the rules give an asset at ObjectPath ("/Game/Fab/Crates/SM_Crate.SM_Crate"):
	 * the deepest rule folder containing its package, compared case-insensitively, as Unreal's
	 * package paths are. Normalized; empty when no rule covers it.
	 */
	static FString ForPath(const FString& ObjectPath, const TArray<FMantleAttributionRule>& Rules);

	/**
	 * An asset's attribution: its package's MantleAttribution tag when it has one (even empty), else
	 * ForPath with the project's rules. Normalized. Loads nothing: an asset that is not loaded is
	 * credited by the rules alone.
	 */
	static FString ForObjectPath(const FString& ObjectPath);

	/**
	 * The body of PUT /assets/attributions: {"attributions":[{"id":...,"attribution":...}, ...]} in
	 * the order given. Every entry is sent, empty ones included: an empty attribution clears one an
	 * earlier publish wrote, so the claims always say what the project says now.
	 */
	static FString MakeRequestBody(const TArray<TPair<FString, FString>>& IdsAndAttributions);
};
