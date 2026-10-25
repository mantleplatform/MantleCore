// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * One place's creator UI files as the client fetched them: `UI/...` path to UTF-8 text. #556 item 5:
 * an image's path (`UI/icons/sword.png`, see MantleDocumentImages) maps instead to where its pixels
 * are, never to the pixels themselves.
 */
using FMantleDocumentFiles = TMap<FString, FString>;

/**
 * #556 item 5: images in an FMantleDocumentFiles. The key is the image's place path, ending in .png
 * or .jpg (case-exact, as the place format's IsUIImagePath says); the value is a reference: a
 * published place's uploaded Texture asset (`mantle-asset:<id>`, drawn from the downloaded place
 * assets), or on a local -MantlePlace run the file's absolute path on disk (`mantle-disk:<path>`,
 * decoded as PNG or JPEG). The document host resolves both; documents reference the image by its
 * path, relative to the file that uses it. In MantleScriptTools so MantleRmlDocumentRules
 * can tell images apart without MantleScript.
 */
namespace MantleDocumentImages
{
	inline constexpr TCHAR AssetPrefix[] = TEXT("mantle-asset:");
	inline constexpr TCHAR DiskPrefix[] = TEXT("mantle-disk:");

	/** True when Path names an image rather than a document or stylesheet. */
	inline bool IsImagePath(const FString& Path)
	{
		return Path.EndsWith(TEXT(".png"), ESearchCase::CaseSensitive) || Path.EndsWith(TEXT(".jpg"), ESearchCase::CaseSensitive);
	}

	/** The file-set value for a published image, whose pixels are the Texture asset AssetId. */
	inline FString ForAsset(const FString& AssetId)
	{
		return FString(AssetPrefix) + AssetId;
	}

	/** The file-set value for an image read from disk at AbsolutePath. */
	inline FString ForDisk(const FString& AbsolutePath)
	{
		return FString(DiskPrefix) + AbsolutePath;
	}
}
