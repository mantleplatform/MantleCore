// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "UObject/SoftObjectPtr.h"

#include "MantleThumbnailChoice.generated.h"

class UTexture2D;

/** How an item's thumbnail is made (#124): the same three choices in every uploader. */
UENUM()
enum class EMantleThumbnailMode : uint8
{
	/** The kind's own render, framed by Mantle. */
	Automatic,
	/** The same render from an angle the creator chose. */
	Angle,
	/** A picture the creator supplies: a PNG file or a Texture2D. */
	Custom
};

/**
 * What a creator chose for an item's thumbnail (#124). Saved with the creator's source item (the place settings actor, the vehicle or
 * weapon source asset, the cosmetic draft), so a re-upload keeps it. FMantleThumbnailResolver turns it into the picture.
 */
USTRUCT()
struct MANTLEEDITOR_API FMantleThumbnailChoice
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Thumbnail")
	EMantleThumbnailMode Mode = EMantleThumbnailMode::Automatic;

	/** The chosen angle's orbit (yaw and pitch, degrees) for kinds shown on an orbit preview. A place keeps its camera on its settings instead. */
	UPROPERTY(EditAnywhere, Category = "Thumbnail")
	FRotator Angle = FRotator::ZeroRotator;

	/** The chosen angle's zoom: 1 is the automatic framing, above 1 is closer. */
	UPROPERTY(EditAnywhere, Category = "Thumbnail", meta = (ClampMin = "0.25", ClampMax = "4"))
	float Zoom = 1.0f;

	/** Custom image: a Texture2D in the project. Used before ImageFile when both are set. */
	UPROPERTY(EditAnywhere, Category = "Thumbnail")
	TSoftObjectPtr<UTexture2D> Texture;

	/** Custom image: a PNG file. */
	UPROPERTY(EditAnywhere, Category = "Thumbnail", meta = (FilePathFilter = "png"))
	FFilePath ImageFile;

	bool operator==(const FMantleThumbnailChoice& Other) const
	{
		return Mode == Other.Mode && Angle.Equals(Other.Angle, 0.001) && FMath::IsNearlyEqual(Zoom, Other.Zoom, 0.0001f)
			&& Texture == Other.Texture && ImageFile.FilePath == Other.ImageFile.FilePath;
	}
	bool operator!=(const FMantleThumbnailChoice& Other) const { return !(*this == Other); }

	/** True when the choice names a custom image (a texture or a file). */
	bool HasCustomImage() const { return !Texture.IsNull() || !ImageFile.FilePath.IsEmpty(); }
};

/** The final picture's size. Every kind is a 512 x 512 square except a place, which is the catalogue's 16:9. */
struct MANTLEEDITOR_API FMantleThumbnailTarget
{
	int32 Width = 512;
	int32 Height = 512;

	/** The library and avatar-shop thumbnail class: 512 x 512 PNG (limits.json avatar content thumbnail). */
	static FMantleThumbnailTarget Square() { return { 512, 512 }; }

	/** A place's catalogue picture (metadata.thumbnail), 1280 x 720. */
	static FMantleThumbnailTarget Wide() { return { 1280, 720 }; }

	bool operator==(const FMantleThumbnailTarget& Other) const { return Width == Other.Width && Height == Other.Height; }
};

/** What Resolve made. Pixels are Target.Width x Target.Height, opaque, top row first, when Error is empty. */
struct MANTLEEDITOR_API FMantleThumbnailResult
{
	TArray<FColor> Pixels;
	/** The picture is made but the creator should know something (a small custom image was enlarged). Plain words. */
	FString Warning;
	/** No picture; why, in plain words. */
	FString Error;
	/** One line for the log and the package notes: where the picture came from. */
	FString Source;

	bool Succeeded() const { return Error.IsEmpty() && Pixels.Num() > 0; }
};

/** How a size compares with the target. */
enum class EMantleThumbnailSize : uint8
{
	/** The cropped part is at least the target's size. */
	Fine,
	/** Between half and the full target: usable, enlarged, with a warning. */
	Enlarged,
	/** Under half the target (256 px for a square): refused. */
	TooSmall
};

/**
 * The one resolver behind every uploader's thumbnail (#124): turns a FMantleThumbnailChoice into the final picture. Automatic and Angle are the
 * kind's own render (the Render callback); Custom is read here, centre-cropped to the target's shape, checked and resized. The pure parts need no
 * renderer and are tested headless.
 */
struct MANTLEEDITOR_API FMantleThumbnailResolver
{
	/**
	 * The kind's render for a choice (Automatic or Angle): pixels of any size with the target's shape. False with the reason in OutError when it cannot
	 * render (no renderer, nothing to show).
	 */
	using FRender = TFunction<bool(const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)>;

	/** The part of a Width x Height image that is kept: the centred rectangle with the target's aspect, as large as fits. Pure. */
	static FIntRect CropRect(int32 Width, int32 Height, const FMantleThumbnailTarget& Target);

	/** How the kept part compares with the target: Fine at or above its size, Enlarged from half of it, else TooSmall. Pure. */
	static EMantleThumbnailSize JudgeSize(int32 Width, int32 Height, const FMantleThumbnailTarget& Target);

	/**
	 * Crops Pixels (Width x Height, top row first) to the target's shape and resizes it to the target: averaging when shrinking, smooth when
	 * enlarging. Transparent pixels are laid over a dark card. False with a plain reason in OutError when it is too small or empty; OutWarning is
	 * set when it was enlarged. Pure.
	 */
	static bool FitToTarget(TConstArrayView<FColor> Pixels, int32 Width, int32 Height, const FMantleThumbnailTarget& Target,
		TArray<FColor>& OutPixels, FString& OutWarning, FString& OutError);

	/** Whether File is inside the project folder (so a custom PNG goes with the project, and with source control). Pure. */
	static bool IsInsideProject(const FString& File);

	/** Reads the choice's custom image (its Texture, else its PNG file) into pixels, top row first. False with a plain reason. */
	static bool LoadCustom(const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);

	/**
	 * The picture for Choice: Custom through LoadCustom and FitToTarget; Automatic and Angle through Render (then FitToTarget when its size is not
	 * the target's). Render may be empty for a caller that only ever resolves Custom.
	 */
	static FMantleThumbnailResult Resolve(const FMantleThumbnailChoice& Choice, const FMantleThumbnailTarget& Target, const FRender& Render);

	/** The picture as PNG bytes for the upload (a role-less texture, thumbnailAssetId); empty when it is not the target's size. */
	static TArray<uint8> ToPng(const FMantleThumbnailResult& Result, const FMantleThumbnailTarget& Target);
};

/** The section's words, shared so every uploader says the same thing. */
struct MANTLEEDITOR_API FMantleThumbnailText
{
	static FText Heading();
	static FText ModeName(EMantleThumbnailMode Mode);
	static FText ModeHelp(EMantleThumbnailMode Mode);
	static FText UseThisView();
	static FText PreviewCaption(const FMantleThumbnailTarget& Target);
};
