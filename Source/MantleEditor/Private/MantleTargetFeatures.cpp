// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTargetFeatures.h"
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantlePlaceAssetConverter.h"
#include "MantleTerrainGrassExport.h"

namespace MantleTargetFeaturesPrivate
{
	const FMantleTargetFeatures* GCurrent = nullptr;
}

const TCHAR* const FMantleTargetFeatures::NeedsNewerClient = TEXT("needs a newer Mantle client");

FMantleTargetFeatures::FMantleTargetFeatures()
{
	for (const FString& Feature : MantleClientFeatures::All())
	{
		Names.Add(Feature);
	}
}

FMantleTargetFeatures FMantleTargetFeatures::All()
{
	return FMantleTargetFeatures();
}

FMantleTargetFeatures FMantleTargetFeatures::None()
{
	FMantleTargetFeatures Out;
	Out.Names.Reset();
	return Out;
}

FMantleTargetFeatures FMantleTargetFeatures::FromNames(const TArray<FString>& InNames)
{
	FMantleTargetFeatures Out = None();
	for (const FString& Name : InNames)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			Out.Names.Add(Trimmed);
		}
	}
	return Out;
}

FMantleTargetFeatures FMantleTargetFeatures::Parse(const FString& List)
{
	TArray<FString> Parts;
	List.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty*/ true);
	for (FString& Part : Parts)
	{
		Part.TrimStartAndEndInline();
		if (Part.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			return All();
		}
	}
	return FromNames(Parts);
}

FString FMantleTargetFeatures::ToArgument() const
{
	TArray<FString> Sorted = Names.Array();
	Sorted.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	return FString::Join(Sorted, TEXT(","));
}

bool FMantleTargetFeatures::WritesFoliage(const FMantlePlaceMaterialDocument& Document)
{
	return Document.WritesFoliage();
}

namespace MantleTargetFeaturesPrivate
{
	/**
	 * A water document as a client without the key reads it: the opaque material near-opaque water published as before the
	 * key. Its fastest normal on the first UV set (the one the old path kept, so the first layer when none moves) with its tiling
	 * and pan as uvTransforms and uvPanning; a world-laid layer has no UV tiling or pan to give, so its texture stays as it is.
	 * The second normal map is dropped, and water's own values (coefficients, layers) go with the key. Water is not metallic, which the
	 * water document does not carry.
	 */
	void DowngradeWater(FMantlePlaceMaterialDocument& Document)
	{
		const FString SecondKey(TEXT("waterNormal2"));
		const bool bFirst = Document.Textures.Contains(TEXT("normal"));
		const bool bSecond = Document.Textures.Contains(SecondKey);
		if (Document.WritesWater() && (bFirst || bSecond))
		{
			auto Speed2 = [](const FMantlePlaceDocumentWaterNormal& Layer) { return FMath::Square(Layer.Panning[0]) + FMath::Square(Layer.Panning[1]); };
			const FMantlePlaceDocumentWaterNormal* Chosen = nullptr;
			bool bChosenSecond = false;
			if (bFirst && !Document.WaterNormal.bWorld)
			{
				Chosen = &Document.WaterNormal;
			}
			if (bSecond && !Document.WaterNormal2.bWorld && (!Chosen || Speed2(Document.WaterNormal2) > Speed2(*Chosen)))
			{
				Chosen = &Document.WaterNormal2;
				bChosenSecond = true;
			}
			if (Chosen)
			{
				if (bChosenSecond)
				{
					// A copy: the map must not be added to from a reference into itself.
					const FString SecondTexture = Document.Textures.FindChecked(SecondKey);
					Document.Textures.Add(TEXT("normal"), SecondTexture);
				}
				if (Chosen->Tiling[0] != 1.0 || Chosen->Tiling[1] != 1.0)
				{
					FMantlePlaceDocumentUvTransform Transform;
					Transform.Scale[0] = Chosen->Tiling[0];
					Transform.Scale[1] = Chosen->Tiling[1];
					Document.UvTransforms.Add(TEXT("normal"), Transform);
				}
				if (Chosen->Panning[0] != 0.0 || Chosen->Panning[1] != 0.0)
				{
					Document.UvPanning.Add(TEXT("normal"), FVector2D(Chosen->Panning[0], Chosen->Panning[1]));
				}
			}
		}
		Document.Textures.Remove(SecondKey);
		Document.bWater = false;
		Document.MetallicFactor = 0.0;
	}
}

bool FMantleTargetFeatures::WritesAdditive(const FMantlePlaceMaterialDocument& Document)
{
	return Document.WritesAdditive();
}

namespace MantleTargetFeaturesPrivate
{
	/**
	 * An additive document as a client without alphaMode additive reads it: translucent blending, with a black base colour
	 * (the glow is the emissive, which stays) and an opacity from the glow, clamp(luminance of emissive factor x strength, 0, 1), so
	 * a bright glow is solid and a faint one shows through. An approximation: translucency blends where additive adds, and a glow
	 * whose strength is above 1 saturates the opacity, but the surface keeps its colour and reads as light over what is behind it.
	 */
	void DowngradeAdditive(FMantlePlaceMaterialDocument& Document)
	{
		const double Luminance = 0.2126 * Document.EmissiveFactor[0] + 0.7152 * Document.EmissiveFactor[1] + 0.0722 * Document.EmissiveFactor[2];
		Document.AlphaMode = TEXT("blend");
		Document.BaseColorFactor[0] = Document.BaseColorFactor[1] = Document.BaseColorFactor[2] = 0.0;
		Document.BaseColorFactor[3] = FMath::Clamp(Luminance * Document.EmissiveStrength, 0.0, 1.0);
		Document.MetallicFactor = 0.0;
		Document.RoughnessFactor = 1.0;
	}
}

bool FMantleTargetFeatures::DowngradeMaterial(FMantlePlaceMaterialDocument& Document) const
{
	bool bChanged = false;
	if (WritesAdditive(Document) && !Reads(MantleClientFeatures::MaterialAdditive))
	{
		MantleTargetFeaturesPrivate::DowngradeAdditive(Document);
		bChanged = true;
	}
	if (Reads(MantleClientFeatures::MaterialFidelity))
	{
		return bChanged;
	}
	if (Document.bFoliage)
	{
		bChanged = WritesFoliage(Document);
		Document.bFoliage = false;
	}
	if (Document.bWater)
	{
		bChanged = Document.WritesWater() || bChanged;
		MantleTargetFeaturesPrivate::DowngradeWater(Document);
	}
	return bChanged;
}

bool FMantleTargetFeatures::DowngradeGrass(FMantleTerrainGrassResult& Result) const
{
	if (Reads(MantleClientFeatures::TerrainGrass) || Result.Entries.Num() == 0)
	{
		return false;
	}
	Result.NeedsNewerClient = Result.TypeNames;
	Result.Entries.Reset();
	Result.TypeNames.Reset();
	Result.TypeWeights.Reset();
	Result.TypeCoverage.Reset();
	Result.TypesInMaterial = 0;
	// What was said about how the grass would have been made is moot; what was left out stays said.
	Result.Warnings.RemoveAll([](const FMantleGrassNote& Note) { return Note.Outcome != EMantleCoverageOutcome::Dropped; });
	return true;
}

void FMantleTargetFeatures::ReportMaterialDowngrades(const FMantlePlaceAssetSet& Set, FMantleCoverage& Out) const
{
	for (const FMantleConvertedMaterial& Material : Set.Materials)
	{
		if (WritesAdditive(Material.Document) && !Reads(MantleClientFeatures::MaterialAdditive))
		{
			Out.Add(MantleCoverageCategory::Materials, EMantleCoverageOutcome::Approximated, Material.Name,
				FString::Printf(TEXT("additive blending left out, a translucent material with an opacity from its glow: %s"), NeedsNewerClient));
		}
		if (Reads(MantleClientFeatures::MaterialFidelity))
		{
			continue;
		}
		if (WritesFoliage(Material.Document))
		{
			Out.Add(MantleCoverageCategory::Materials, EMantleCoverageOutcome::Approximated, Material.Name,
				FString::Printf(TEXT("foliage shading left out, a plain masked two-sided material: %s"), NeedsNewerClient));
		}
		if (Material.Document.WritesWater())
		{
			Out.Add(MantleCoverageCategory::Materials, EMantleCoverageOutcome::Approximated, Material.Name,
				FString::Printf(TEXT("water shading left out, an opaque material with its fastest normal panned: %s"), NeedsNewerClient));
		}
	}
}

const FMantleTargetFeatures& FMantleTargetFeatures::Current()
{
	static const FMantleTargetFeatures Everything;
	return MantleTargetFeaturesPrivate::GCurrent ? *MantleTargetFeaturesPrivate::GCurrent : Everything;
}

FMantleTargetFeaturesScope::FMantleTargetFeaturesScope(const FMantleTargetFeatures& Features)
	: Held(Features), Previous(MantleTargetFeaturesPrivate::GCurrent)
{
	MantleTargetFeaturesPrivate::GCurrent = &Held;
}

FMantleTargetFeaturesScope::~FMantleTargetFeaturesScope()
{
	MantleTargetFeaturesPrivate::GCurrent = Previous;
}
