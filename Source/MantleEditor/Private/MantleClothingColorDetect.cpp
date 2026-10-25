// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClothingColorDetect.h"

#include "MantleClothingColorMeasure.h"
#include "MantleGlbWriter.h"

#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"

namespace MantleClothingColorDetectPrivate
{
	uint8 ChannelOf(const FColor& Pixel, uint8 Channel)
	{
		return Channel == 0 ? Pixel.R : Channel == 1 ? Pixel.G : Channel == 2 ? Pixel.B : Pixel.A;
	}

	/** Whether channels A and B are on (at least one half) at the same used texels, all but a sliver of edge. */
	bool SameArea(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> Used, uint8 A, uint8 B)
	{
		int64 Count = 0, Differ = 0;
		for (int32 Index = 0; Index < Pixels.Num() && Index < Used.Num(); ++Index)
		{
			if (!Used[Index]) continue;
			++Count;
			Differ += (ChannelOf(Pixels[Index], A) >= 128) != (ChannelOf(Pixels[Index], B) >= 128) ? 1 : 0;
		}
		return Count > 0 && Differ <= Count / 200;
	}

	/** Near-0 and near-1 values snap to the ends; the anti-aliased edge between them is kept. */
	uint8 Snap(uint8 Value)
	{
		const float V = Value / 255.0f;
		return V < FMantleClothingColorDetect::LowThreshold ? 0 : V > FMantleClothingColorDetect::HighThreshold ? 255 : Value;
	}

	/** The "main" region (channel 0), numbered by MainNumber, which it advances. */
	FMantleClothingColorRegion MakeMainRegion(int32& MainNumber)
	{
		FMantleClothingColorRegion Main;
		Main.Id = MainNumber == 1 ? FString(TEXT("main")) : FString::Printf(TEXT("main_%d"), MainNumber);
		Main.Label = MainNumber == 1 ? FString(TEXT("Main")) : FString::Printf(TEXT("Main %d"), MainNumber);
		Main.Channel = 0;
		++MainNumber;
		return Main;
	}
}

bool FMantleClothingColorDetect::IsMaskParameterName(const FString& Name)
{
	FString Key = Name.ToLower();
	Key.ReplaceInline(TEXT(" "), TEXT(""));
	Key.ReplaceInline(TEXT("_"), TEXT(""));
	Key.ReplaceInline(TEXT("-"), TEXT(""));
	static const TCHAR* Exact[] = {
		TEXT("mask"), TEXT("masks"), TEXT("id"), TEXT("ids"), TEXT("idmap"), TEXT("idmask"), TEXT("region"), TEXT("regions"),
		TEXT("regionmask"), TEXT("regionmap"), TEXT("colormask"), TEXT("colourmask"), TEXT("colorid"), TEXT("colourid"),
		TEXT("colormap"), TEXT("dyemask"), TEXT("tintmask"), TEXT("materialid"), TEXT("matid") };
	for (const TCHAR* Candidate : Exact)
	{
		if (Key == Candidate) return true;
	}
	// "Garment Color Mask", "Shirt_ID_Mask": a name ending in a region word, never opacity or animated wrinkle maps.
	if (Key.Contains(TEXT("opacity")) || Key.Contains(TEXT("alpha")) || Key.Contains(TEXT("wrinkle")) || Key.Contains(TEXT("animated")))
	{
		return false;
	}
	return Key.EndsWith(TEXT("colormask")) || Key.EndsWith(TEXT("colourmask")) || Key.EndsWith(TEXT("regionmask")) || Key.EndsWith(TEXT("idmask"))
		|| Key.EndsWith(TEXT("idmap")) || Key.EndsWith(TEXT("dyemask")) || Key.EndsWith(TEXT("tintmask"));
}

bool FMantleClothingColorDetect::IsLooseMaskParameterName(const FString& Name)
{
	if (IsMaskParameterName(Name)) return true;
	FString Key = Name.ToLower();
	Key.ReplaceInline(TEXT(" "), TEXT(""));
	Key.ReplaceInline(TEXT("_"), TEXT(""));
	Key.ReplaceInline(TEXT("-"), TEXT(""));
	static const TCHAR* Never[] = { TEXT("opacity"), TEXT("alpha"), TEXT("wrinkle"), TEXT("animated"), TEXT("body"), TEXT("hide"), TEXT("hidden") };
	for (const TCHAR* Word : Never)
	{
		if (Key.Contains(Word)) return false;
	}
	return Key.Contains(TEXT("mask")) || Key.EndsWith(TEXT("id")) || Key.EndsWith(TEXT("ids"));
}

TArray<TPair<FString, UTexture*>> FMantleClothingColorDetect::FindMaskTextures(UMaterialInterface* Material)
{
	TArray<TPair<FString, UTexture*>> Strict, Loose;
	if (!Material) return Strict;
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Material->GetAllTextureParameterInfo(Infos, Ids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		const FString Name = Info.Name.ToString();
		if (!IsLooseMaskParameterName(Name)) continue;
		UTexture* Texture = nullptr;
		if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Texture) || !Texture) continue;
		// One texture behind two parameters is tried once.
		if (Strict.ContainsByPredicate([Texture](const TPair<FString, UTexture*>& Pair) { return Pair.Value == Texture; })
			|| Loose.ContainsByPredicate([Texture](const TPair<FString, UTexture*>& Pair) { return Pair.Value == Texture; })) continue;
		(IsMaskParameterName(Name) ? Strict : Loose).Emplace(Name, Texture);
	}
	// A map the material samples directly, not through a parameter ("Mask_01" wired straight into a lerp): its asset name decides.
	TArray<UTexture*> Used;
	Material->GetUsedTextures(Used);
	for (UTexture* Texture : Used)
	{
		if (!Texture) continue;
		FString Name = Texture->GetName();
		if (Name.StartsWith(TEXT("T_"))) Name.RightChopInline(2);
		if (!IsLooseMaskParameterName(Name)) continue;
		if (Strict.ContainsByPredicate([Texture](const TPair<FString, UTexture*>& Pair) { return Pair.Value == Texture; })
			|| Loose.ContainsByPredicate([Texture](const TPair<FString, UTexture*>& Pair) { return Pair.Value == Texture; })) continue;
		Loose.Emplace(Texture->GetName(), Texture);
	}
	Strict.Append(Loose);
	return Strict;
}

bool FMantleClothingColorDetect::DetectWhole(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size, FMantleClothingColorDetection& InOut,
	TArray<FColor>& OutCleanMask, TArray<uint8>& OutUsed, int32& MainNumber, FString& OutError)
{
	InOut.Areas.Reset();
	InOut.SourceChannels.Reset();
	OutCleanMask.Reset();
	OutUsed.Reset();
	if (!FMantleClothingColorMeasure::MarkUsedTexels(Mesh, MaterialIndex, Size, OutUsed, OutError)) return false;
	OutCleanMask.Init(FColor(255, 0, 0, 255), Size.X * Size.Y);
	InOut.Areas.Add(MantleClothingColorDetectPrivate::MakeMainRegion(MainNumber));
	InOut.Why.Reset();
	return true;
}

FMantleAreaChannelScore FMantleClothingColorDetect::ScoreChannel(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> Used, FIntPoint Size, uint8 Channel)
{
	using namespace MantleClothingColorDetectPrivate;
	FMantleAreaChannelScore Score;
	Score.Channel = Channel;
	const int64 Count = static_cast<int64>(Size.X) * Size.Y;
	if (Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != Count || Used.Num() != Count) return Score;
	const uint8 Low = static_cast<uint8>(FMath::FloorToInt(LowThreshold * 255.0f));
	const uint8 High = static_cast<uint8>(FMath::CeilToInt(HighThreshold * 255.0f));
	int64 UsedCount = 0, Binary = 0, On = 0, Mid = 0, Crossings = 0;
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			const int32 Index = Y * Size.X + X;
			if (!Used[Index]) continue;
			const uint8 Value = ChannelOf(Pixels[Index], Channel);
			++UsedCount;
			if (Value < Low || Value > High) ++Binary; else ++Mid;
			const bool bOn = Value >= 128;
			On += bOn ? 1 : 0;
			// Crossings between used neighbours only: a UV island's border is not an edge of the map.
			if (X + 1 < Size.X && Used[Index + 1] && (ChannelOf(Pixels[Index + 1], Channel) >= 128) != bOn) ++Crossings;
			if (Y + 1 < Size.Y && Used[Index + Size.X] && (ChannelOf(Pixels[Index + Size.X], Channel) >= 128) != bOn) ++Crossings;
		}
	}
	if (UsedCount == 0) return Score;
	Score.Binary = static_cast<float>(static_cast<double>(Binary) / UsedCount);
	Score.Coverage = static_cast<float>(static_cast<double>(On) / UsedCount);
	Score.EdgeWidth = Crossings > 0 ? static_cast<float>(static_cast<double>(Mid) / Crossings) : (Mid > 0 ? TNumericLimits<float>::Max() : 0.0f);
	Score.bAccepted = Score.Binary >= MinBinary && Score.Coverage > MinCoverage && Score.Coverage < MaxCoverage && Crossings > 0 && Score.EdgeWidth <= MaxEdgeWidth;
	return Score;
}

void FMantleClothingColorDetect::Choose(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> Used, FIntPoint Size, FMantleClothingColorDetection& InOut)
{
	InOut.Scores.Reset();
	InOut.SourceChannels.Reset();
	for (uint8 Channel = 0; Channel < 3; ++Channel)
	{
		FMantleAreaChannelScore Score = ScoreChannel(Pixels, Used, Size, Channel);
		// A greyscale map repeats one area in every channel: an accepted channel that matches one already taken is the same area.
		if (Score.bAccepted && InOut.SourceChannels.ContainsByPredicate([&](uint8 Taken) { return MantleClothingColorDetectPrivate::SameArea(Pixels, Used, Taken, Channel); }))
		{
			Score.bAccepted = false;
		}
		InOut.Scores.Add(Score);
		if (Score.bAccepted) InOut.SourceChannels.Add(Channel);
	}
	if (InOut.SourceChannels.Num() > 2)
	{
		// Every channel a clean region: a many-colour ID map, whose colours mix channels. Not split in v1.
		InOut.SourceChannels.Reset();
		InOut.Why = TEXT("its colour map uses many colours, which Mantle can't split into areas yet");
	}
	else if (InOut.SourceChannels.IsEmpty())
	{
		InOut.Why = TEXT("its texture has no clearly separated areas");
	}
	else
	{
		InOut.Why.Reset();
	}
}

TArray<FColor> FMantleClothingColorDetect::Repack(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> SourceChannels)
{
	using namespace MantleClothingColorDetectPrivate;
	TArray<FColor> Clean;
	Clean.SetNumUninitialized(Pixels.Num());
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		const uint8 First = SourceChannels.Num() > 0 ? Snap(ChannelOf(Pixels[Index], SourceChannels[0])) : 0;
		// Where both are on, the first wins: the areas never overlap, so the three always sum to one.
		const uint8 Second = SourceChannels.Num() > 1 ? FMath::Min<uint8>(Snap(ChannelOf(Pixels[Index], SourceChannels[1])), 255 - First) : 0;
		Clean[Index] = FColor(static_cast<uint8>(255 - First - Second), First, Second, 255);
	}
	return Clean;
}

bool FMantleClothingColorDetect::Detect(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size, TConstArrayView<FColor> SourceMask,
	FMantleClothingColorDetection& InOut, TArray<FColor>& OutCleanMask, TArray<uint8>& OutUsed, int32& MainNumber, int32& AccentNumber, FString& OutError)
{
	InOut.Areas.Reset();
	OutCleanMask.Reset();
	OutUsed.Reset();
	if (SourceMask.Num() != static_cast<int64>(Size.X) * Size.Y) { OutError = TEXT("The colour map and its size disagree."); return false; }
	if (!FMantleClothingColorMeasure::MarkUsedTexels(Mesh, MaterialIndex, Size, OutUsed, OutError)) return false;
	Choose(SourceMask, OutUsed, Size, InOut);
	if (InOut.SourceChannels.IsEmpty()) return true;
	OutCleanMask = Repack(SourceMask, InOut.SourceChannels);
	// Main is whatever no accent covers; left out when the accents cover (nearly) everything the material uses.
	int64 UsedCount = 0, MainOn = 0;
	for (int32 Index = 0; Index < OutUsed.Num(); ++Index)
	{
		if (!OutUsed[Index]) continue;
		++UsedCount;
		MainOn += OutCleanMask[Index].R >= 128 ? 1 : 0;
	}
	if (UsedCount > 0 && static_cast<double>(MainOn) / UsedCount > MinCoverage)
	{
		InOut.Areas.Add(MantleClothingColorDetectPrivate::MakeMainRegion(MainNumber));
	}
	for (int32 Index = 0; Index < InOut.SourceChannels.Num(); ++Index)
	{
		FMantleClothingColorRegion Accent;
		Accent.Id = FString::Printf(TEXT("accent_%d"), AccentNumber);
		Accent.Label = FString::Printf(TEXT("Accent %d"), AccentNumber);
		Accent.Channel = static_cast<uint8>(1 + Index);
		++AccentNumber;
		InOut.Areas.Add(Accent);
	}
	return true;
}

TArray<FMantleClothingColorRegion> FMantleClothingColorDetect::ApplyChoices(const FMantleClothingColorDetection& Detection, TConstArrayView<FMantleClothingColorChoice> Choices)
{
	TArray<FMantleClothingColorRegion> Regions;
	for (const FMantleClothingColorRegion& Area : Detection.Areas)
	{
		const FMantleClothingColorChoice* Choice = Choices.FindByPredicate([&Detection, &Area](const FMantleClothingColorChoice& Entry)
			{ return Entry.MaterialName == Detection.MaterialName && Entry.RegionId == Area.Id; });
		// An area shared by several sections (one material drawn twice) is one control: the choice made on any of them holds.
		if (!Choice) Choice = Choices.FindByPredicate([&Area](const FMantleClothingColorChoice& Entry) { return Entry.RegionId == Area.Id; });
		if (Choice && !Choice->bEnabled) continue;
		FMantleClothingColorRegion Region = Area;
		if (Choice)
		{
			FString Label = Choice->Label.TrimStartAndEnd();
			bool bControl = false;
			for (const TCHAR Character : Label) bControl |= Character < 32 || Character == 127;
			if (!Label.IsEmpty() && Label.Len() <= 64 && !bControl) Region.Label = MoveTemp(Label);
		}
		Regions.Add(MoveTemp(Region));
	}
	return Regions;
}

FColor FMantleClothingColorDetect::AverageColor(TConstArrayView<FColor> Albedo, TConstArrayView<FColor> Mask, TConstArrayView<uint8> Used, uint8 Channel)
{
	using namespace MantleClothingColorDetectPrivate;
	double Weight = 0, R = 0, G = 0, B = 0;
	const int32 Count = FMath::Min3(Albedo.Num(), Mask.Num(), Used.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!Used[Index]) continue;
		const double W = ChannelOf(Mask[Index], Channel) / 255.0;
		if (W <= 0) continue;
		const FLinearColor Linear = FLinearColor::FromSRGBColor(Albedo[Index]);
		Weight += W; R += W * Linear.R; G += W * Linear.G; B += W * Linear.B;
	}
	if (Weight <= 0) return FColor(128, 128, 128, 255);
	FColor Result = FLinearColor(static_cast<float>(R / Weight), static_cast<float>(G / Weight), static_cast<float>(B / Weight)).ToFColor(true);
	Result.A = 255;
	return Result;
}

FColor FMantleClothingColorDetect::HighlightColor(int32 AreaIndex)
{
	static const FColor Palette[] = {
		FColor(230, 70, 50), FColor(40, 140, 235), FColor(250, 205, 30), FColor(60, 190, 90),
		FColor(175, 80, 220), FColor(255, 140, 20), FColor(20, 200, 200), FColor(235, 80, 170) };
	return Palette[FMath::Abs(AreaIndex) % UE_ARRAY_COUNT(Palette)];
}
