// Copyright Joshua Eger. All Rights Reserved.
// The script API as the editor sees it: the committed manifest the game's API registry generates
// (Resources/ScriptApi/mantle_api.json, a copy of schema/script/generated/), so the graph
// catalogue, the script API version check and the creator type files work without MantleScript.
// Mantle.Script.Api.ManifestCopiesAreCurrent keeps both copies equal to the live registry.

#pragma once

#include "CoreMinimal.h"
#include "MantleScriptSide.h"

struct FMantleApiManifestParam
{
	FString Name;
	/** Luau-style type annotation: "string", "number", "Vector3", "Entity?", "{Entity}", "(Entity) -> ()". */
	FString Type;
	bool bOptional = false;
};

struct FMantleApiManifestMethod
{
	FString Name; // canonical PascalCase
	EMantleSide Side = EMantleSide::Both;
	TArray<FMantleApiManifestParam> Params;
	bool bVariadic = false;
	FString Returns = TEXT("()");
	FString Doc;
	/** Implemented by each language's Kit rather than natively (e.g. Signal:Wait). */
	bool bKit = false;
	bool bDeprecated = false;
	FString DeprecatedReason;
	/** The minor of the API's major that added this member; 0 for the base surface. */
	int32 IntroducedMinor = 0;
};

struct FMantleApiManifestProperty
{
	FString Name;
	FString Type;
	FString Doc;
	EMantleSide GetSide = EMantleSide::Both;
	bool bSettable = false;
	EMantleSide SetSide = EMantleSide::Server;
	bool bDeprecated = false;
	FString DeprecatedReason;
	int32 IntroducedMinor = 0;
};

struct FMantleApiManifestSignal
{
	FString Name;
	/** Handler parameters, e.g. "(player: Player)". */
	FString Args;
	FString Doc;
	EMantleSide Side = EMantleSide::Both;
	bool bDeprecated = false;
	FString DeprecatedReason;
	int32 IntroducedMinor = 0;
};

struct MANTLEEDITOR_API FMantleApiManifestClass
{
	FString Name;
	FString Doc;
	/** Services are singletons exposed as globals (World, Players...). */
	bool bService = false;
	/** The minor that added the whole class; every member is at least this new. 0 for the base surface. */
	int32 IntroducedMinor = 0;

	TArray<FMantleApiManifestMethod> Methods;
	TArray<FMantleApiManifestProperty> Properties;
	TArray<FMantleApiManifestSignal> Signals;

	/** The minor that introduced Member (canonical name, case-exact): the newer of the member's own and
	 *  the class's. INDEX_NONE when the class has no such member. */
	int32 MemberIntroducedMinor(const FString& Member) const;
};

class MANTLEEDITOR_API FMantleApiManifest
{
public:
	/** The plugin's manifest, read once. Empty (Major 0) with an error logged if it is missing or broken. */
	static const FMantleApiManifest& Get();

	/** Reads a manifest's JSON text. False with the reason in OutError when it isn't one. */
	static bool Parse(const FString& Json, FMantleApiManifest& Out, FString& OutError);

	/** Resources/ScriptApi in the MantleCore plugin: mantle_api.json and the creator type files
	 *  (mantle.d.luau, mantle.d.ts, mantle.pyi). Empty when the plugin can't be found. */
	static FString ResourceDirectory();

	/** The script API version the manifest describes (the newest this editor's places can record). */
	int32 Major = 0;
	int32 Minor = 0;

	TArray<FMantleApiManifestClass> Classes;

	/** The class named Name, ignoring case as the game's registry does; null when there is none. */
	const FMantleApiManifestClass* FindClass(const FString& Name) const;
};
