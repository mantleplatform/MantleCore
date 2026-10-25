// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainGrassExport.h"

#include "AssetCompilingManager.h"
#include "Engine/StaticMesh.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeEditTypes.h"
#include "LandscapeGrassType.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceLimits.h"
#include "MantlePng.h"
#include "MantleRuntime.h"
#include "MantleSha256.h"
#include "MantleTerrainSampling.h"
#include "MantleTargetFeatures.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionLandscapeGrassOutput.h"
#include "Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Materials/MaterialExpressionLandscapeLayerWeight.h"
#include "Materials/MaterialInterface.h"
#include "MaterialCachedData.h"
#include "Misc/App.h"
#include "RHIGlobals.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainGrassExportPrivate
{
	/** The grass type's weights as a share of the landscape's samples that have any, 0 to 100. */
	double GrassCoveragePercent(const TArray<uint8>& Weights)
	{
		if (Weights.IsEmpty())
		{
			return 0.0;
		}
		int64 NonZero = 0;
		for (const uint8 Value : Weights)
		{
			NonZero += Value != 0 ? 1 : 0;
		}
		return 100.0 * static_cast<double>(NonZero) / static_cast<double>(Weights.Num());
	}

	/** How many entries belong to the grass type at Index of Result.TypeNames. */
	int32 CountVarieties(const FMantleTerrainGrassResult& Result, const int32 Index)
	{
		int32 Varieties = 0;
		for (const FMantleGrassEntryDraft& Entry : Result.Entries)
		{
			Varieties += Entry.TypeName.Equals(Result.TypeNames[Index], ESearchCase::CaseSensitive) ? 1 : 0;
		}
		return Varieties;
	}

	FString GrassLabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** The layer a grass input reads: the Layer Sample or Layer Weight it is wired to, or the first one upstream of it. */
	bool FindGrassInputLayer(const FExpressionInput& Input, bool& bOutDirect, FName& OutLayer)
	{
		if (!Input.Expression)
		{
			return false;
		}
		TArray<const UMaterialExpression*> Queue;
		TSet<const UMaterialExpression*> Seen;
		TArray<int32> Depth;
		const UMaterialExpression* Start = Input.Expression;
		Queue.Add(Start);
		Depth.Add(0);
		Seen.Add(Start);
		for (int32 Index = 0; Index < Queue.Num() && Index < 256; ++Index)
		{
			const UMaterialExpression* Expression = Queue[Index];
			FName Layer;
			if (const UMaterialExpressionLandscapeLayerSample* Sample = Cast<UMaterialExpressionLandscapeLayerSample>(Expression))
			{
				Layer = Sample->ParameterName;
			}
			else if (const UMaterialExpressionLandscapeLayerWeight* Weight = Cast<UMaterialExpressionLandscapeLayerWeight>(Expression))
			{
				Layer = Weight->ParameterName;
			}
			if (!Layer.IsNone())
			{
				OutLayer = Layer;
				bOutDirect = Depth[Index] == 0;
				return true;
			}
			const int32 NextDepth = Depth[Index] + 1;
			for (int32 InputIndex = 0; InputIndex < 64; ++InputIndex)
			{
				const FExpressionInput* Next = Expression->GetInput(InputIndex);
				if (!Next)
				{
					break;
				}
				const UMaterialExpression* NextExpression = Next->Expression;
				if (NextExpression && !Seen.Contains(NextExpression))
				{
					Seen.Add(NextExpression);
					Queue.Add(NextExpression);
					Depth.Add(NextDepth);
				}
			}
		}
		return false;
	}

	/** Every grass type the landscape's material declares, by its name in the material, grass type overrides applied. */
	void CollectMaterialGrassTypes(const ALandscapeProxy* Landscape, TMap<FName, ULandscapeGrassType*>& Out)
	{
		const UMaterialInterface* Material = Landscape->GetLandscapeMaterial();
		const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		if (!Base)
		{
			return;
		}
		const TMap<FLandscapeGrassName, TObjectPtr<ULandscapeGrassType>> Overrides = Landscape->GetGrassTypesOverrides();
		for (const TPair<FName, TObjectPtr<ULandscapeGrassType>>& Pair : Base->GetCachedExpressionData().NamedGrassTypes)
		{
			ULandscapeGrassType* Type = Pair.Value.Get();
			if (const TObjectPtr<ULandscapeGrassType>* Override = Overrides.Find(FLandscapeGrassName(Pair.Key)))
			{
				Type = Override->Get();
			}
			Out.Add(Pair.Key, Type);
		}
	}

	/**
	 * The engine's own grass weights over Source's grid: builds every component's grass maps (the exact
	 * result of the material's grass output, rendered) when they are missing, then copies each component's
	 * weights into one grid per grass name. False when the engine could not build any, so the caller
	 * falls back to paint layers.
	 */
	bool ReadEngineWeights(const ALandscapeProxy* Landscape, const FMantleTerrainSource& Source, TMap<FName, ULandscapeGrassType*>& OutTypes,
		TMap<FName, TArray<uint8>>& OutWeights, TArray<FMantleGrassNote>& Warnings)
	{
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		UWorld* World = Landscape->GetWorld();
		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;
		if (!Info || !World || !Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY)
			|| MaxX - MinX + 1 != Source.Width || MaxY - MinY + 1 != Source.Height)
		{
			return false;
		}

		TArray<ULandscapeComponent*> Components;
		Info->ForEachLandscapeProxy([&Components](ALandscapeProxy* Proxy)
		{
			for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
			{
				if (Component)
				{
					Components.Add(Component);
				}
			}
			return true;
		});

		// The types by name, as each component sees them (a proxy can override a grass type).
		bool bAllRegistered = true;
		for (ULandscapeComponent* Component : Components)
		{
			Component->UpdateGrassTypes();
			bAllRegistered = bAllRegistered && Component->IsRegistered();
			for (const TPair<FName, TObjectPtr<ULandscapeGrassType>>& Pair : Component->GetNamedGrassTypes())
			{
				if (!OutTypes.Contains(Pair.Key))
				{
					OutTypes.Add(Pair.Key, Pair.Value.Get());
				}
			}
		}
		if (OutTypes.IsEmpty())
		{
			return true;
		}

		// Rendering the grass maps needs the renderer and components the grass builder tracks.
		if (FApp::CanEverRender() && !GUsingNullRHI && bAllRegistered)
		{
			if (ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>())
			{
				// Textures and the landscape material's shaders must be ready, or the grass map renders blank.
				FAssetCompilingManager::Get().FinishAllCompilation();
				Subsystem->BuildGrassMaps(UE::Landscape::EBuildFlags::None);
			}
		}

		int32 Built = 0;
		int32 Unbuilt = 0;
		const int32 Cells = Source.Width * Source.Height;
		for (ULandscapeComponent* Component : Components)
		{
			const FLandscapeComponentGrassData& Data = Component->GrassData.Get();
			if (Data.NumElements < 0)
			{
				++Unbuilt;
				continue;
			}
			++Built;
			const int32 Stride = Component->ComponentSizeQuads + 1;
			if (Data.NumElements == 0 || Data.NumElements != Stride * Stride)
			{
				continue; // no grass here (the engine drops all-zero maps), or a size this reader does not know
			}
			for (const TPair<FName, int32>& Pair : Data.WeightOffsets)
			{
				if (!OutTypes.Contains(Pair.Key) || Pair.Value < 0 || Pair.Value + Data.NumElements > Data.HeightWeightData.Num())
				{
					continue;
				}
				TArray<uint8>& Grid = OutWeights.FindOrAdd(Pair.Key);
				if (Grid.IsEmpty())
				{
					Grid.SetNumZeroed(Cells);
				}
				const uint8* Source8 = Data.HeightWeightData.GetData() + Pair.Value;
				for (int32 Y = 0; Y < Stride; ++Y)
				{
					const int32 GridY = Component->SectionBaseY + Y - MinY;
					if (GridY < 0 || GridY >= Source.Height)
					{
						continue;
					}
					for (int32 X = 0; X < Stride; ++X)
					{
						const int32 GridX = Component->SectionBaseX + X - MinX;
						if (GridX >= 0 && GridX < Source.Width)
						{
							uint8& Cell = Grid[GridY * Source.Width + GridX];
							Cell = FMath::Max(Cell, Source8[Y * Stride + X]);
						}
					}
				}
			}
		}
		if (Built == 0)
		{
			OutWeights.Reset();
			return false;
		}
		if (Unbuilt > 0)
		{
			Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, FString(), FString::Printf(TEXT("%d of %d landscape components have no grass map built, so they have no grass."), Unbuilt, Components.Num())));
		}
		return true;
	}

	/**
	 * The fallback: each grass input wired to a Landscape Layer Sample or Layer Weight takes that paint
	 * layer's weights (an input that only has one upstream is approximated by it, with a warning; one with
	 * none is left out).
	 */
	void ReadLayerWeights(const ALandscapeProxy* Landscape, const FMantleTerrainSource& Source, TMap<FName, ULandscapeGrassType*>& OutTypes,
		TMap<FName, TArray<uint8>>& OutWeights, TArray<FMantleGrassNote>& Warnings)
	{
		OutTypes.Reset();
		OutWeights.Reset();
		CollectMaterialGrassTypes(Landscape, OutTypes);
		const UMaterialInterface* Material = Landscape->GetLandscapeMaterial();
		const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		if (!Base)
		{
			return;
		}
		for (const TObjectPtr<UMaterialExpression>& Expression : Base->GetExpressions())
		{
			const UMaterialExpressionLandscapeGrassOutput* Output = Cast<UMaterialExpressionLandscapeGrassOutput>(Expression.Get());
			if (!Output)
			{
				continue;
			}
			for (const FGrassInput& Input : Output->GrassTypes)
			{
				if (!OutTypes.Contains(Input.Name) || OutWeights.Contains(Input.Name))
				{
					continue;
				}
				bool bDirect = false;
				FName Layer;
				if (!FindGrassInputLayer(Input.Input, bDirect, Layer))
				{
					Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, Input.Name.ToString(), FString::Printf(TEXT("Grass type '%s' is not driven by a paint layer the editor can follow, so its grass is left out."), *Input.Name.ToString())));
					OutTypes.Remove(Input.Name);
					continue;
				}
				if (!bDirect)
				{
					Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Input.Name.ToString(), FString::Printf(TEXT("Grass type '%s' is computed from the material, so it is approximated by the weights of paint layer '%s'."),
						*Input.Name.ToString(), *Layer.ToString())));
				}
				const int32 LayerIndex = Source.LayerNames.Find(Layer);
				if (Source.LayerWeights.IsValidIndex(LayerIndex) && Source.LayerWeights[LayerIndex].Num() == Source.Width * Source.Height)
				{
					OutWeights.Add(Input.Name, Source.LayerWeights[LayerIndex]);
				}
			}
		}
	}
}

double FMantleTerrainGrassExport::DensityPerSquareMetre(const float EngineDensity)
{
	return static_cast<double>(EngineDensity) / 100.0;
}

FString FMantleTerrainGrassExport::GrassMapKey(const AActor* Landscape, const int32 MapIndex)
{
	return FString::Printf(TEXT("grassmap:%s:%d"), Landscape ? *Landscape->GetPathName() : TEXT(""), MapIndex);
}

void FMantleTerrainGrassExport::SlotToMapChannel(const int32 Slot, int32& OutMap, int32& OutChannel)
{
	OutMap = Slot / 4;
	OutChannel = Slot % 4;
}

const TCHAR* FMantleTerrainGrassExport::ChannelName(const int32 Channel)
{
	switch (Channel)
	{
	case 1: return TEXT("g");
	case 2: return TEXT("b");
	case 3: return TEXT("a");
	default: return TEXT("r");
	}
}

bool FMantleTerrainGrassExport::ReadVariety(const FGrassVariety& Variety, const FString& Label, FMantleTerrainGrass& Out, TArray<FMantleGrassNote>& Warnings)
{
	Out = FMantleTerrainGrass();

	double Density = DensityPerSquareMetre(Variety.GetDensity());
	if (Density <= 0.0)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, Label, FString::Printf(TEXT("%s has a grass density of 0, so it grows nothing and is left out."), *Label)));
		return false;
	}
	if (Density > FMantlePlaceLimits::MaxTerrainGrassDensity)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s has %g instances per square metre; Mantle grows at most %g, so it gets that."), *Label, Density, FMantlePlaceLimits::MaxTerrainGrassDensity)));
		Density = FMantlePlaceLimits::MaxTerrainGrassDensity;
	}
	Out.Density = Density;

	// One uniform scale range: the engine's Uniform scaling is exactly ScaleX; a free or XY-locked variety
	// keeps its X range, which is all Mantle has.
	double MinScale = Variety.ScaleX.Min;
	double MaxScale = Variety.ScaleX.Max;
	if (MinScale > MaxScale)
	{
		Swap(MinScale, MaxScale);
	}
	const double ScaleLow = FMantlePlaceLimits::MinScatterScale;
	const double ScaleHigh = FMantlePlaceLimits::MaxScatterScale;
	if (MinScale < ScaleLow || MaxScale > ScaleHigh)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s has a scale of %g to %g; Mantle keeps %g to %g, so it is clamped."), *Label, MinScale, MaxScale, ScaleLow, ScaleHigh)));
		MinScale = FMath::Clamp(MinScale, ScaleLow, ScaleHigh);
		MaxScale = FMath::Clamp(MaxScale, ScaleLow, ScaleHigh);
	}
	Out.MinScale = MinScale;
	Out.MaxScale = MaxScale;
	auto SameRange = [&Variety](const FFloatInterval& Other)
	{
		return FMath::IsNearlyEqual(Other.Min, Variety.ScaleX.Min, 0.0001f) && FMath::IsNearlyEqual(Other.Max, Variety.ScaleX.Max, 0.0001f);
	};
	if ((Variety.Scaling == EGrassScaling::Free && (!SameRange(Variety.ScaleY) || !SameRange(Variety.ScaleZ)))
		|| (Variety.Scaling == EGrassScaling::LockXY && !SameRange(Variety.ScaleZ)))
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s scales its axes separately; Mantle scales every axis alike, by the X range."), *Label)));
	}
	if (Variety.bWeightAttenuatesMaxScale)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s makes instances smaller where the weight is low; Mantle does not, so they are full size."), *Label)));
	}
	if (Variety.AllowedDensityRange.Min > 0.0f || Variety.AllowedDensityRange.Max < 1.0f)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s only grows where the weight is between %g and %g; Mantle grows it wherever the weight is above 0."),
			*Label, Variety.AllowedDensityRange.Min, Variety.AllowedDensityRange.Max)));
	}

	// Cull distances: the engine's 0 means no limit; Mantle needs an end above 0 and at most its limit.
	double CullStart = Variety.GetStartCullDistance();
	double CullEnd = Variety.GetEndCullDistance();
	const double CullMax = FMantlePlaceLimits::MaxTerrainGrassCullDistance;
	if (CullEnd <= 0.0)
	{
		CullEnd = 10000.0;
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s has no end cull distance; Mantle draws it out to %g cm."), *Label, CullEnd)));
	}
	else if (CullEnd > CullMax)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s is drawn to %g cm; Mantle draws grass to at most %g cm, so it is cut there."), *Label, CullEnd, CullMax)));
		CullEnd = CullMax;
	}
	// The engine's start distance of 0 (or one at the end) means no fade: instances are cut at the end. Mantle
	// fades from its start, so that is a start just short of the end.
	if (CullStart > CullEnd)
	{
		Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, Label, FString::Printf(TEXT("%s starts to fade at %g cm, past where it ends (%g cm), so it is cut at the end."), *Label, CullStart, CullEnd)));
	}
	if (CullStart <= 0.0 || CullStart >= CullEnd)
	{
		CullStart = FMath::Max(CullEnd - 1.0, 0.0);
	}
	Out.CullStart = CullStart;
	Out.CullEnd = CullEnd;

	Out.bAlignToSurface = Variety.AlignToSurface;
	Out.bRandomYaw = Variety.RandomRotation;
	Out.bCastShadow = Variety.bCastDynamicShadow;
	return true;
}

int32 FMantleTerrainGrassExport::AppendEntries(const ULandscapeGrassType* Type, const FString& TypeName, const int32 Slot, FMantleTerrainGrassResult& Out)
{
	int32 Added = 0;
	if (!Type || Slot < 0 || Slot >= FMantlePlaceLimits::MaxTerrainGrassMaps * 4)
	{
		return 0;
	}
	TArray<FString> LeftOut;
	for (const FGrassVariety& Variety : Type->GrassVarieties)
	{
		const UStaticMesh* Mesh = Variety.GrassMesh.Get();
		const FString Label = FString::Printf(TEXT("Grass type '%s' mesh '%s'"), *TypeName, Mesh ? *Mesh->GetName() : TEXT("(none)"));
		if (!Mesh)
		{
			Out.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, TypeName, FString::Printf(TEXT("Grass type '%s' has a variety with no mesh, so it is left out."), *TypeName)));
			continue;
		}
		if (Out.Entries.Num() >= FMantlePlaceLimits::MaxTerrainGrass)
		{
			LeftOut.Add(Mesh->GetName());
			continue;
		}
		FMantleTerrainGrass Look;
		if (!ReadVariety(Variety, Label, Look, Out.Warnings))
		{
			continue;
		}
		FMantleGrassEntryDraft& Draft = Out.Entries.AddDefaulted_GetRef();
		Draft.TypeName = TypeName;
		Draft.Mesh = Mesh;
		for (const TObjectPtr<UMaterialInterface>& Material : Variety.OverrideMaterials)
		{
			Draft.Materials.Add(Material.Get());
		}
		SlotToMapChannel(Slot, Draft.Map, Draft.Channel);
		Draft.Look = Look;
		Draft.Look.Channel = Draft.Channel;
		++Added;
	}
	if (LeftOut.Num() > 0)
	{
		Out.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, TypeName, FString::Printf(TEXT("A Terrain publishes at most %d grass entries, so %d of grass type '%s' are left out: %s."),
			FMantlePlaceLimits::MaxTerrainGrass, LeftOut.Num(), *TypeName, *FString::Join(LeftOut, TEXT(", ")))));
	}
	return Added;
}

TArray<FColor> FMantleTerrainGrassExport::MakeMap(const TArray<TArray<uint8>>& Weights, TConstArrayView<int32> Indices, const int32 Width, const int32 Height, const FIntPoint Size)
{
	using namespace MantleTerrainGrassExportPrivate;

	TArray<FColor> Pixels;
	if (Size.X <= 0 || Size.Y <= 0 || Width < 2 || Height < 2)
	{
		return Pixels;
	}
	const int32 Count = FMath::Min(Indices.Num(), 4);
	Pixels.SetNumZeroed(Size.X * Size.Y);
	for (int32 TexelY = 0; TexelY < Size.Y; ++TexelY)
	{
		// As FMantleTerrainExport::MakeWeightmap: the texel's centre as a fraction of the terrain, then in samples.
		const double SampleY = (TexelY + 0.5) / Size.Y * (Height - 1);
		for (int32 TexelX = 0; TexelX < Size.X; ++TexelX)
		{
			const double SampleX = (TexelX + 0.5) / Size.X * (Width - 1);
			uint8 Channels[4] = { 0, 0, 0, 0 };
			for (int32 Channel = 0; Channel < Count; ++Channel)
			{
				const int32 Index = Indices[Channel];
				if (Weights.IsValidIndex(Index) && Weights[Index].Num() == Width * Height)
				{
					Channels[Channel] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(MantleTerrainSampling::Bilinear(Weights[Index], Width, Height, SampleX, SampleY)), 0, 255));
				}
			}
			Pixels[TexelY * Size.X + TexelX] = FColor(Channels[0], Channels[1], Channels[2], Channels[3]);
		}
	}
	return Pixels;
}

FMantleTerrainGrassResult FMantleTerrainGrassExport::Gather(const AActor* Landscape, const FMantleTerrainSource& Source, const bool bLog)
{
	using namespace MantleTerrainGrassExportPrivate;

	FMantleTerrainGrassResult Result;
	Result.Width = Source.Width;
	Result.Height = Source.Height;
	const ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Landscape);
	if (!Proxy || Source.Width < 2 || Source.Height < 2)
	{
		return Result;
	}

	TMap<FName, ULandscapeGrassType*> Types;
	TMap<FName, TArray<uint8>> Weights;
	if (!ReadEngineWeights(Proxy, Source, Types, Weights, Result.Warnings))
	{
		Result.bEngineWeights = false;
		Result.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Approximated, FString(),
			TEXT("The editor could not build the landscape's grass maps (no renderer, or the landscape is not loaded the way the grass builder needs), so grass follows the paint layers its material names instead.")));
		ReadLayerWeights(Proxy, Source, Types, Weights, Result.Warnings);
	}
	Result.TypesInMaterial = Types.Num();

	TArray<FName> Names;
	Types.GetKeys(Names);
	Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	TArray<FString> NoSlot;
	for (const FName& Name : Names)
	{
		const FString TypeName = Name.ToString();
		const ULandscapeGrassType* Type = Types.FindRef(Name);
		const TArray<uint8>* Grid = Weights.Find(Name);
		const double Coverage = Grid ? GrassCoveragePercent(*Grid) : 0.0;
		if (!Type)
		{
			Result.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, TypeName, FString::Printf(TEXT("Grass type '%s' has no grass type asset assigned in the material, so it is left out."), *TypeName)));
			continue;
		}
		if (Coverage <= 0.0)
		{
			if (bLog)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_GRASS_TYPE name=%s coverage=0.0%% varieties=%d slot=none (no weight anywhere)"), *TypeName, Type->GrassVarieties.Num());
			}
			continue;
		}
		const int32 Slot = Result.TypeNames.Num();
		if (Slot >= FMantlePlaceLimits::MaxTerrainGrassMaps * 4)
		{
			NoSlot.Add(TypeName);
			continue;
		}
		if (AppendEntries(Type, TypeName, Slot, Result) > 0)
		{
			Result.TypeNames.Add(TypeName);
			Result.TypeWeights.Add(*Grid);
			Result.TypeCoverage.Add(Coverage);
		}
	}
	if (NoSlot.Num() > 0)
	{
		Result.Warnings.Add(FMantleGrassNote(EMantleCoverageOutcome::Dropped, FString::Join(NoSlot, TEXT(", ")),
			FString::Printf(TEXT("A Terrain publishes at most %d grass maps (%d grass types), so these grass types are left out: %s."),
				FMantlePlaceLimits::MaxTerrainGrassMaps, FMantlePlaceLimits::MaxTerrainGrassMaps * 4, *FString::Join(NoSlot, TEXT(", ")))));
	}

	// A target client that cannot read grass (a local preview for an older client): left out, and named for the coverage report.
	FMantleTargetFeatures::Current().DowngradeGrass(Result);

	if (bLog)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_GRASS_EXPORT types=%d entries=%d maps=%d path=%s"),
			Result.TypeNames.Num(), Result.Entries.Num(), Result.NumMaps(), Result.bEngineWeights ? TEXT("engine") : TEXT("layers"));
		for (int32 Index = 0; Index < Result.TypeNames.Num(); ++Index)
		{
			const int32 Varieties = CountVarieties(Result, Index);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_GRASS_TYPE name=%s coverage=%.1f%% varieties=%d slot=map%d.%s"),
				*Result.TypeNames[Index], Result.TypeCoverage[Index], Varieties, Index / 4, ChannelName(Index % 4));
		}
		for (const FMantleGrassNote& Warning : Result.Warnings)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_GRASS_WARNING %s: %s"), Warning.Outcome == EMantleCoverageOutcome::Dropped ? TEXT("dropped") : TEXT("approximated"), *Warning.Text);
		}
	}
	return Result;
}

void FMantleTerrainGrassExport::AddAssets(const AActor* Landscape, FMantlePlaceDependencies& Dependencies, const FString& Label)
{
	using namespace MantleTerrainGrassExportPrivate;

	FMantleTerrainSource Source;
	FString Problem;
	if (!FMantleTerrainExport::ReadLandscape(Landscape, Source, Problem))
	{
		return; // Check and Export name it
	}
	const FMantleTerrainGrassResult Result = Gather(Landscape, Source, /*bLog*/ false);
	for (const FMantleGrassEntryDraft& Entry : Result.Entries)
	{
		Dependencies.AddMesh(Entry.Mesh, Label);
		for (UMaterialInterface* Material : Entry.Materials)
		{
			Dependencies.AddMaterial(Material, Label);
		}
	}
}

bool FMantleTerrainGrassExport::ConvertMaps(const AActor* Landscape, const FMantleTerrainSource& Source, const FMantlePlaceAssetLimits& Limits, FMantlePlaceAssetSet& Out)
{
	using namespace MantleTerrainGrassExportPrivate;

	const FMantleTerrainGrassResult Result = Gather(Landscape, Source, /*bLog*/ true);
	const FString Label = GrassLabelOf(Landscape);
	for (const FMantleGrassNote& Warning : Result.Warnings)
	{
		Out.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, Label, Warning.Text,
			TEXT("It still publishes. Nothing to do unless the grass matters to how your place looks; if it does, change the grass type or the landscape material.") });
	}
	for (int32 MapIndex = 0; MapIndex < Result.NumMaps(); ++MapIndex)
	{
		TArray<int32> Indices;
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			Indices.Add(MapIndex * 4 + Channel < Result.TypeNames.Num() ? MapIndex * 4 + Channel : INDEX_NONE);
		}
		// Lossless (no role, so the cook keeps the PNG), halved until the moderation classifier takes it, as the weightmap is.
		FIntPoint Size = FMantleTerrainExport::WeightmapSize(Source.Width, Source.Height, Limits.MaxTextureSize);
		TArray<uint8> Png = FMantlePng::EncodeRGBA8(Size.X, Size.Y, MakeMap(Result.TypeWeights, Indices, Source.Width, Source.Height, Size));
		while (Png.Num() > Limits.MaxTextureBytes && (Size.X > 64 || Size.Y > 64))
		{
			Size = FIntPoint(FMath::Max(Size.X / 2, 1), FMath::Max(Size.Y / 2, 1));
			Png = FMantlePng::EncodeRGBA8(Size.X, Size.Y, MakeMap(Result.TypeWeights, Indices, Source.Width, Source.Height, Size));
		}
		if (Png.IsEmpty() || Png.Num() > Limits.MaxTextureBytes)
		{
			Out.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
				FString::Printf(TEXT("Landscape '%s''s grass could not be made into an image Mantle takes."), *Label),
				TEXT("Use fewer grass types on the landscape material, then publish again.") });
			return false;
		}
		FMantleConvertedTexture Texture;
		Texture.Key = GrassMapKey(Landscape, MapIndex);
		Texture.Name = FString::Printf(TEXT("%s (grass %d)"), *Label, MapIndex + 1);
		Texture.Format = TEXT("png");
		Texture.Width = Size.X;
		Texture.Height = Size.Y;
		Texture.Hash = FMantleSha256::AssetHash(Png);
		Texture.Bytes = MoveTemp(Png);
		Out.AddTexture(MoveTemp(Texture));
	}
	return true;
}

void FMantleTerrainGrassExport::ReportCoverage(FMantleCoverage& Out, const FMantleTerrainGrassResult& Result)
{
	using namespace MantleTerrainGrassExportPrivate;
	for (const FString& TypeName : Result.NeedsNewerClient)
	{
		Out.Add(MantleCoverageCategory::LandscapeGrass, EMantleCoverageOutcome::Dropped, TypeName,
			FString::Printf(TEXT("grass left out: %s"), FMantleTargetFeatures::NeedsNewerClient));
	}
	// What was exported, per grass type: kept when its weights are the engine's own, approximated when they are a paint layer's.
	for (int32 Index = 0; Index < Result.TypeNames.Num(); ++Index)
	{
		const int32 Varieties = CountVarieties(Result, Index);
		Out.Add(MantleCoverageCategory::LandscapeGrass, Result.bEngineWeights ? EMantleCoverageOutcome::Kept : EMantleCoverageOutcome::Approximated,
			Result.TypeNames[Index],
			FString::Printf(TEXT("%d varieties over %.1f%% of the landscape%s"), Varieties, Result.TypeCoverage[Index],
				Result.bEngineWeights ? TEXT("") : TEXT(", weights from a paint layer")));
	}
	for (const FMantleGrassNote& Note : Result.Warnings)
	{
		Out.Add(MantleCoverageCategory::LandscapeGrass, Note.Outcome, Note.Name, Note.Text);
	}
	if (Result.TypesInMaterial > 0 && Result.TypeNames.IsEmpty() && Result.Warnings.IsEmpty())
	{
		Out.Add(MantleCoverageCategory::LandscapeGrass, EMantleCoverageOutcome::Info, FString(), TEXT("the landscape's grass types grow nowhere on it"));
	}
}

FMantleTerrainGrass FMantleTerrainGrassExport::MakeGrass(const FMantleGrassEntryDraft& Draft, const FString& MapId, const FString& MeshId, const TArray<FString>& MaterialIds)
{
	FMantleTerrainGrass Out = Draft.Look;
	Out.Map = MapId;
	Out.Channel = Draft.Channel;
	Out.Mesh = MeshId;
	Out.Materials = MaterialIds;
	return Out;
}

void FMantleTerrainGrassExport::WriteJson(const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>& Writer, const TArray<FMantleTerrainGrass>& Grass)
{
	if (Grass.IsEmpty())
	{
		return;
	}
	Writer->WriteArrayStart(TEXT("grass"));
	for (const FMantleTerrainGrass& Entry : Grass)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("map"), Entry.Map);
		Writer->WriteValue(TEXT("channel"), FString(ChannelName(Entry.Channel)));
		Writer->WriteValue(TEXT("mesh"), Entry.Mesh);
		bool bAnyMaterial = false;
		for (const FString& Material : Entry.Materials)
		{
			bAnyMaterial = bAnyMaterial || !Material.IsEmpty();
		}
		if (bAnyMaterial)
		{
			Writer->WriteArrayStart(TEXT("materials"));
			for (const FString& Material : Entry.Materials)
			{
				if (Material.IsEmpty())
				{
					Writer->WriteNull(); // an unset slot
				}
				else
				{
					Writer->WriteValue(Material);
				}
			}
			Writer->WriteArrayEnd();
		}
		Writer->WriteValue(TEXT("density"), Entry.Density);
		Writer->WriteArrayStart(TEXT("scale"));
		Writer->WriteValue(Entry.MinScale);
		Writer->WriteValue(Entry.MaxScale);
		Writer->WriteArrayEnd();
		Writer->WriteValue(TEXT("alignToSurface"), Entry.bAlignToSurface);
		Writer->WriteValue(TEXT("randomYaw"), Entry.bRandomYaw);
		Writer->WriteArrayStart(TEXT("cullDistance"));
		Writer->WriteValue(Entry.CullStart);
		Writer->WriteValue(Entry.CullEnd);
		Writer->WriteArrayEnd();
		Writer->WriteValue(TEXT("castShadow"), Entry.bCastShadow);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
}
