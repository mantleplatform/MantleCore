// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * #51: the plain words the hair head-fit preview shows, decided from what the fit measured. Pure: the export fills the
 * input, the panel shows the output. The creator never types a scale or an offset; they read these and press "Looks good"
 * or "Adjust" (which uploads the hair as authored, so they can place it by hand).
 */
/** The geometry behind the before/after preview, in the export's space (Unreal, cm): the hair as authored and as fitted, and the head it was fitted to. */
struct MANTLEEDITOR_API FMantleHairFitMeshes
{
	TArray<FVector3f> BeforePositions;
	TArray<FVector3f> AfterPositions;
	/** Triangles (three indices each) into the hair positions. */
	TArray<uint32> HairTriangles;
	TArray<FVector3f> HeadPositions;
	TArray<uint32> HeadTriangles;
	bool IsValid() const { return AfterPositions.Num() > 0 && BeforePositions.Num() == AfterPositions.Num() && HeadPositions.Num() > 0; }

	/**
	 * The head as the hair as authored sees it: the head moved by the size and shift that took the hair from BeforePositions to AfterPositions
	 * (a uniform scale about the centroids; the scalp fit's small moves average out). Drawn with BeforePositions it shows the hair on a head
	 * it was made for, so the "before" picture shows hair on a head, not a bald head with the hair far away. BeforePositions must be upright
	 * (turned if the file was Y-up): this does not rotate.
	 */
	TArray<FVector3f> HeadInHairFrame() const;
};

struct MANTLEEDITOR_API FMantleHairFitReport
{
	/** Shown above the before/after picture whenever the fit ran. */
	static const TCHAR* Headline() { return TEXT("We sized your hair to fit the standard head"); }
	static const TCHAR* ConfirmLabel() { return TEXT("Looks good"); }
	static const TCHAR* AdjustLabel() { return TEXT("Adjust"); }

	/** The share of cards whose roots the whole-piece fit seated on the scalp (percent) under which the fit is called low confidence. */
	static constexpr double LowSeatedPercent = 35.0;
	/** The share of card vertices left inside the skin (percent) over which the fit is called low confidence. */
	static constexpr double LowInsidePercent = 2.0;

	struct FInput
	{
		/** The whole-piece fit found a size and position that keeps the hair out of the head (FMantleHairPieceFit::MaxDeepInsideFraction). */
		bool bKeepsOutOfHead = true;
		/** The scalp fit asked the creator to check (it had to move a lot of hair a long way). */
		bool bScalpCheckFit = false;
		/** Cards only (no cap): the share of cards whose roots the whole-piece fit seated on the scalp, percent; negative = not measured. */
		double RootsSeatedPercent = -1.0;
		int32 CardVertices = 0;
		int32 CardInsideAfter = 0;
		bool bHadCap = false;
		/** The mesh has a section that reads like a head ("head", "face", "skin", "body"), baked into the hair file. */
		bool bBakedHead = false;
		FString BakedHeadSection;
	};

	struct FResult
	{
		FString Headline;
		bool bLowConfidence = false;
		bool bBakedHead = false;
		/** Plain sentences after the headline: the low-confidence and baked-head messages. */
		TArray<FString> Messages;
	};

	static FResult Describe(const FInput& Input);

	/** True when a material/section name reads like a head, not hair: head, face, skin or body, and not a cap. */
	static bool IsBakedHeadName(const FString& SectionName);
};
