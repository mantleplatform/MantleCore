// Copyright Joshua Eger. All Rights Reserved.
// Starter script templates for New Script. Content follows Mantle's registered API
// (MantleApiCore.cpp): method names, casing and the Tween goal shape all match what
// World/Entity/Players/State/Net/UI really expose.

#pragma once

#include "CoreMinimal.h"
#include "MantleScriptRow.h"

class UMantleScriptEdGraph;

/** A starter recipe "New Script..." and Mantle.NewScript can generate. */
enum class EMantleScriptTemplateKind : uint8
{
	Empty,
	KillBrick,
	Coins,
	RoundTimer,
	Door
};

struct FMantleNewScriptRequest
{
	EMantleScriptRowSide Side = EMantleSide::Server;
	EMantleScriptEditorLanguage Language = EMantleScriptEditorLanguage::Luau;
	EMantleScriptTemplateKind Template = EMantleScriptTemplateKind::Empty;
	/** File base name, no extension: "MyBricks". */
	FString Name;
};

/**
 * Generates template source and writes new script files under a Scripts/ root. Pure logic, no
 * Slate -- shared by SMantleNewScriptDialog's Create button and the Mantle.NewScript console
 * command, so both do exactly the same thing.
 */
class FMantleScriptTemplates
{
public:
	/** Languages offered by "New Script...", including PocketPy Python source. */
	static TArray<EMantleScriptEditorLanguage> NewScriptLanguages();
	static TArray<EMantleScriptTemplateKind> AllTemplates();
	static TArray<EMantleScriptRowSide> AllSides();

	static FString SideFolderName(EMantleScriptRowSide Side);
	static FString TemplateDisplayName(EMantleScriptTemplateKind Template);
	static FString LanguageDisplayName(EMantleScriptEditorLanguage Language);
	static FString FileExtension(EMantleScriptEditorLanguage Language);

	static bool ParseSide(const FString& Text, EMantleScriptRowSide& OutSide);
	static bool ParseLanguage(const FString& Text, EMantleScriptEditorLanguage& OutLanguage);
	static bool ParseTemplate(const FString& Text, EMantleScriptTemplateKind& OutTemplate);

	/** The template source for Request, in Request.Language. */
	static FString Generate(const FMantleNewScriptRequest& Request);

	/**
	 * Creates Scripts/<Side>/<Name>.<ext> under ScriptsRoot (making Scripts/<Side>/ if needed)
	 * and writes the generated template into it. Fails if the file already exists, or Name /
	 * ScriptsRoot is invalid. Returns the full path written, or an empty string with OutError
	 * set on failure.
	 */
	static FString CreateScriptFile(const FString& ScriptsRoot, const FMantleNewScriptRequest& Request, FString& OutError);

    /** Populate a fresh in-memory graph using the same builder as New Script asset creation. */
    static void PopulateGraph(UMantleScriptEdGraph& Graph, EMantleScriptTemplateKind Template, EMantleScriptRowSide Side);

	/**
	 * Creates Scripts/<Side>/<Name> (a UMantleScriptGraph asset) under ScriptsPackageRoot (the
	 * package-path twin of ScriptsRoot, FMantleScriptDiscovery::CurrentLevelScriptsPackageRoot)
	 * with Req.Template's starter graph loaded in. Fails if the asset already exists, or Name /
	 * ScriptsPackageRoot is invalid. Returns the new asset's package name, or an empty string with
	 * OutError set on failure.
	 */
	static FString CreateGraphAsset(const FString& ScriptsPackageRoot, const FMantleNewScriptRequest& Request, FString& OutError);
};
