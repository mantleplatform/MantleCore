// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UObject;

/**
 * Where a public submission came from (#614 stage 7): the source asset's path in the creator's project and the
 * name of the file it was imported from. The plugin sends it with PUBLIC avatar items and library items so a
 * moderator reviewing a copyright report can see what the creator uploaded. It is never sent for a private item.
 *
 * Privacy: only the file's NAME is kept, never the folders it sat in on the creator's disk.
 * Both texts are cleaned (control characters removed, cut to the API's limits) before anything is sent.
 */
struct MANTLEEDITOR_API FMantleProvenance
{
	/** The API's limits, in UTF-8 bytes (the strictest reading of "at most 512 / 256"). */
	static constexpr int32 MaxSourcePathBytes = 512;
	static constexpr int32 MaxImportFilenameBytes = 256;

	/** The source asset's object path, e.g. /Game/Packs/Foo/SK_Foo. */
	FString SourcePath;

	/** The import file's name only, e.g. Foo.fbx. */
	FString ImportFilename;

	/** True when there is nothing to send. */
	bool IsEmpty() const { return SourcePath.IsEmpty() && ImportFilename.IsEmpty(); }

	/** A provenance of cleaned texts: Make("/Game/A/B", "C:\\Art\\Pack\\B.fbx") is {"/Game/A/B", "B.fbx"}. */
	static FMantleProvenance Make(const FString& InSourcePath, const FString& RawImportFilename);

	/**
	 * Text without control characters (C0, DEL, C1, line and paragraph separators), trimmed, and cut so its UTF-8 form is at
	 * most MaxUtf8Bytes (never in the middle of a character).
	 */
	static FString Clean(const FString& Text, int32 MaxUtf8Bytes);

	/** The file name of a path with either kind of separator ("C:\\Art\\B.fbx" and "/home/a/B.fbx" are both "B.fbx"); the input without separators is returned as it is. Not cleaned or cut. */
	static FString FileNameOf(const FString& Path);

	/**
	 * An object path without its repeated asset name: "/Game/A/B.B" is "/Game/A/B" (what the Content Browser calls the asset's
	 * path); anything else is returned as it is.
	 */
	static FString AssetPathOf(const FString& ObjectPath);

	/** Clean(FileNameOf(RawFilename), MaxImportFilenameBytes). */
	static FString CleanFilename(const FString& RawFilename);

	/**
	 * The clean file name Source was imported from, read from its AssetImportData (a skeletal or static mesh, an animation, a
	 * groom: whatever carries an AssetImportData property), or empty when it has none, was not imported, or Source is null.
	 */
	static FString ImportFilenameOf(const UObject* Source);

	/** The provenance of an asset in the project: its object path and ImportFilenameOf. Empty for null. */
	static FMantleProvenance FromAsset(const UObject* Source);

	/** {"sourcePath", "importFilename"}, both always present (empty when unknown). Null when IsEmpty(). */
	TSharedPtr<FJsonObject> ToJson() const;

	/** Adds "provenance" to Body when bPublic and there is something to send; otherwise leaves Body alone. */
	void AddTo(FJsonObject& Body, bool bPublic) const;
};
