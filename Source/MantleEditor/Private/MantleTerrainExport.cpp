// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainExport.h"

#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "MantleWaterBodyExport.h"
#include "MantleCoverage.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePng.h"
#include "MantleRuntime.h"
#include "MantleTerrainSampling.h"
#include "Async/ParallelFor.h"
#include "Materials/MaterialInterface.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainExportPrivate
{
	FString TerrainLabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	bool TerrainAnyNonZero(const TArray<uint8>& Values)
	{
		for (const uint8 Value : Values)
		{
			if (Value != 0)
			{
				return true;
			}
		}
		return false;
	}

	/** The rise in cm over the run in cm along each axis at sample (X, Y): central differences, one-sided at the edges. */
	void TerrainSlopeAt(const FMantleTerrainSource& Source, const int32 X, const int32 Y, double& OutSlopeX, double& OutSlopeY)
	{
		const int32 X0 = FMath::Max(X - 1, 0);
		const int32 X1 = FMath::Min(X + 1, Source.Width - 1);
		const int32 Y0 = FMath::Max(Y - 1, 0);
		const int32 Y1 = FMath::Min(Y + 1, Source.Height - 1);
		OutSlopeX = (static_cast<double>(Source.Heights[Y * Source.Width + X1]) - Source.Heights[Y * Source.Width + X0]) * Source.HeightScale
			/ FMath::Max((X1 - X0) * Source.SpacingX, UE_DOUBLE_SMALL_NUMBER);
		OutSlopeY = (static_cast<double>(Source.Heights[Y1 * Source.Width + X]) - Source.Heights[Y0 * Source.Width + X]) * Source.HeightScale
			/ FMath::Max((Y1 - Y0) * Source.SpacingY, UE_DOUBLE_SMALL_NUMBER);
	}

	/**
	 * The surface at sample (X, Y) of Source for a material program: its world position (Source.Transform of the sample's
	 * place on the grid, the height in cm) and the frame of its surface, from the heights' central differences exactly as
	 * BakeSlopeMask takes them: the normal (-dz/dx, -dz/dy, 1) and the tangent (1, 0, dz/dx) and binormal (0, 1, dz/dy), normalised.
	 */
	FMantleSurfaceSample TerrainSurfaceAt(const FMantleTerrainSource& Source, const int32 X, const int32 Y)
	{
		double SlopeX = 0.0;
		double SlopeY = 0.0;
		TerrainSlopeAt(Source, X, Y, SlopeX, SlopeY);

		const double Height = Source.Heights[Y * Source.Width + X] * Source.HeightScale + Source.HeightOffset;
		FMantleSurfaceSample Sample;
		Sample.WorldPosition = Source.Transform.TransformPosition(FVector(X * Source.SpacingX, Y * Source.SpacingY, Height));
		Sample.LandscapeSample = FVector2D(Source.FirstSample.X + X, Source.FirstSample.Y + Y);
		const FQuat Rotation = Source.Transform.GetRotation();
		Sample.Normal = Rotation.RotateVector(FVector(-SlopeX, -SlopeY, 1.0).GetSafeNormal());
		Sample.Tangent = Rotation.RotateVector(FVector(1.0, 0.0, SlopeX).GetSafeNormal());
		Sample.Binormal = Rotation.RotateVector(FVector(0.0, 1.0, SlopeY).GetSafeNormal());
		return Sample;
	}

	/** Two weightmaps carry at most this many layers: four channels each. */
	constexpr int32 TerrainChannelsPerMap = 4;
	static_assert(FMantlePlaceLimits::MaxTerrainLayers <= 2 * TerrainChannelsPerMap, "the weightmaps' channels carry at most 8 layers");

	/** What MakeWeightmap and MakeWeightmaps put in each channel. */
	struct FTerrainWeightPlan
	{
		/** (source layer, channel) pairs: a source layer's weight is added to its channel. */
		TArray<TPair<int32, int32>> Contributions;

		/** Channels in use: layers 0 to Count - 1. */
		int32 Count = 0;

		/** The channel of the baked slope overlay, or INDEX_NONE. */
		int32 SlopeChannel = INDEX_NONE;
	};

	bool TerrainHasWeights(const FMantleTerrainSource& Source, const int32 SourceIndex)
	{
		return Source.LayerWeights.IsValidIndex(SourceIndex) && Source.LayerWeights[SourceIndex].Num() == Source.Width * Source.Height;
	}

	/**
	 * Size.X x Size.Y texels of Plan's channels, each the bilinear weight at the texel's centre, summing to 255 over all
	 * channels. The painted layers are normalised among themselves (all of one channel where nothing is painted) and scaled
	 * by 1 - the slope overlay's weight; the overlay takes the rest. Channels 0 to 3 go to OutFirst, 4 to 7 to OutSecond
	 * (left empty up to 4 channels). SlopeMask is BakeSlopeMask's, empty for no overlay.
	 */
	void BuildTerrainWeightTexels(const FMantleTerrainSource& Source, const FTerrainWeightPlan& Plan, const TArray<uint8>& SlopeMask, const FIntPoint Size,
		TArray<FColor>& OutFirst, TArray<FColor>& OutSecond)
	{
		OutFirst.Reset();
		OutSecond.Reset();
		if (Size.X <= 0 || Size.Y <= 0 || Source.Width < 2 || Source.Height < 2)
		{
			return;
		}
		const int32 Count = FMath::Clamp(Plan.Count, 1, FMantlePlaceLimits::MaxTerrainLayers);
		const bool bSlope = Plan.SlopeChannel >= 0 && Plan.SlopeChannel < Count && SlopeMask.Num() == Source.Width * Source.Height;
		// Where nothing is painted the first channel that is not the overlay's takes all of the painted share.
		const int32 DefaultChannel = bSlope && Plan.SlopeChannel == 0 ? FMath::Min(1, Count - 1) : 0;

		OutFirst.SetNumZeroed(Size.X * Size.Y);
		if (Count > TerrainChannelsPerMap)
		{
			OutSecond.SetNumZeroed(Size.X * Size.Y);
		}
		for (int32 TexelY = 0; TexelY < Size.Y; ++TexelY)
		{
			// The texel's centre as a fraction of the terrain, then in samples: UV0 is 0 at the first sample
			// and 1 at the last, and a texture's texel centres sit at (i + 0.5) / size.
			const double SampleY = (TexelY + 0.5) / Size.Y * (Source.Height - 1);
			for (int32 TexelX = 0; TexelX < Size.X; ++TexelX)
			{
				const double SampleX = (TexelX + 0.5) / Size.X * (Source.Width - 1);
				double Painted[FMantlePlaceLimits::MaxTerrainLayers] = {};
				double Sum = 0.0;
				for (const TPair<int32, int32>& Contribution : Plan.Contributions)
				{
					if (Contribution.Value < 0 || Contribution.Value >= Count)
					{
						continue;
					}
					const double Weight = MantleTerrainSampling::Bilinear(Source.LayerWeights[Contribution.Key], Source.Width, Source.Height, SampleX, SampleY);
					Painted[Contribution.Value] += Weight;
					Sum += Weight;
				}
				const double Slope = bSlope ? FMath::Clamp(MantleTerrainSampling::Bilinear(SlopeMask, Source.Width, Source.Height, SampleX, SampleY) / 255.0, 0.0, 1.0) : 0.0;

				double Share[FMantlePlaceLimits::MaxTerrainLayers] = {};
				if (Sum > 0.0)
				{
					for (int32 Channel = 0; Channel < Count; ++Channel)
					{
						Share[Channel] = Painted[Channel] / Sum * (1.0 - Slope);
					}
				}
				else
				{
					Share[DefaultChannel] = 1.0 - Slope;
				}
				if (bSlope)
				{
					Share[Plan.SlopeChannel] = Slope;
				}

				// Normalised to 255, the rounding remainder given to the heaviest layer so the sum is exact.
				uint8 Channels[FMantlePlaceLimits::MaxTerrainLayers] = {};
				int32 Total = 0;
				int32 Heaviest = 0;
				for (int32 Channel = 0; Channel < Count; ++Channel)
				{
					Channels[Channel] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Share[Channel] * 255.0), 0, 255));
					Total += Channels[Channel];
					Heaviest = Share[Channel] > Share[Heaviest] ? Channel : Heaviest;
				}
				Channels[Heaviest] = static_cast<uint8>(FMath::Clamp(Channels[Heaviest] + (255 - Total), 0, 255));

				OutFirst[TexelY * Size.X + TexelX] = FColor(Channels[0], Channels[1], Channels[2], Channels[3]);
				if (OutSecond.Num() > 0)
				{
					OutSecond[TexelY * Size.X + TexelX] = FColor(Channels[4], Channels[5], Channels[6], Channels[7]);
				}
			}
		}
	}
}

TArray<AActor*> FMantleTerrainExport::CollectLandscapes(UWorld* World)
{
	TArray<AActor*> Out;
	if (!World)
	{
		return Out;
	}
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Out.Add(*It);
		}
	}
	return Out;
}

FString FMantleTerrainExport::HeightfieldKey(const AActor* Landscape)
{
	return TEXT("heightfield:") + (Landscape ? Landscape->GetPathName() : FString());
}

FString FMantleTerrainExport::WeightmapKey(const AActor* Landscape)
{
	return TEXT("weightmap:") + (Landscape ? Landscape->GetPathName() : FString());
}

FString FMantleTerrainExport::Weightmap2Key(const AActor* Landscape)
{
	return Weightmap2KeyForPath(Landscape ? Landscape->GetPathName() : FString());
}

FString FMantleTerrainExport::Weightmap2KeyForPath(const FString& LandscapePath)
{
	return TEXT("weightmap2:") + LandscapePath;
}

bool FMantleTerrainExport::ReadLandscape(const AActor* Actor, FMantleTerrainSource& Out, FString& OutProblem)
{
	using namespace MantleTerrainExportPrivate;

	Out = FMantleTerrainSource();
	const ALandscape* Landscape = Cast<ALandscape>(Actor);
	ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
	if (!Info)
	{
		OutProblem = TEXT("has no landscape data to publish.");
		return false;
	}
	int32 MinX = 0;
	int32 MinY = 0;
	int32 MaxX = 0;
	int32 MaxY = 0;
	if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY) || MaxX <= MinX || MaxY <= MinY)
	{
		OutProblem = TEXT("has no landscape components loaded, so there is nothing to publish.");
		return false;
	}

	// The landscape's own space is one unit a sample and 1/128 of a unit a height value; its scale
	// turns that into cm. A mirrored or flattened landscape has no heightfield Mantle can draw.
	const FVector Scale = Landscape->GetActorScale3D();
	if (Scale.X <= 0.0 || Scale.Y <= 0.0 || Scale.Z <= 0.0)
	{
		OutProblem = FString::Printf(TEXT("is scaled (%g, %g, %g); Mantle publishes a landscape whose scale is above zero on every axis."),
			Scale.X, Scale.Y, Scale.Z);
		return false;
	}
	if (Scale.X < MinSpacingCm || Scale.X > MaxSpacingCm || Scale.Y < MinSpacingCm || Scale.Y > MaxSpacingCm)
	{
		OutProblem = FString::Printf(TEXT("has its samples %g x %g cm apart (its X and Y scale); Mantle publishes %g to %g cm."),
			Scale.X, Scale.Y, MinSpacingCm, MaxSpacingCm);
		return false;
	}
	const double HeightScale = Scale.Z * LANDSCAPE_ZSCALE;
	if (HeightScale > MaxHeightScaleCm || 32768.0 * HeightScale > MaxHeightOffsetCm)
	{
		OutProblem = FString::Printf(TEXT("is scaled %g along Z; Mantle publishes a Z scale of at most %g."), Scale.Z, MaxHeightScaleCm / LANDSCAPE_ZSCALE);
		return false;
	}

	Out.Width = MaxX - MinX + 1;
	Out.Height = MaxY - MinY + 1;
	Out.FirstSample = FIntPoint(MinX, MinY);
	Out.SpacingX = Scale.X;
	Out.SpacingY = Scale.Y;
	Out.HeightScale = HeightScale;
	Out.HeightOffset = -32768.0 * HeightScale;

	// Water Bodies carve a landscape by rendering a brush into an edit layer, and the landscape merges its layers into its own
	// textures on the GPU and reads them back. Those textures are what is read below, so a landscape whose layers are out of date
	// (a body moved, a level just opened) is brought up to date first.
	Out.WaterBodiesCarving = FMantleWaterBodyExport::CountBodiesCarving(Landscape);
	if (Out.WaterBodiesCarving > 0 && !Landscape->IsUpToDate())
	{
		const_cast<ALandscape*>(Landscape)->ForceUpdateLayersContent();
		Out.bCarveRefreshed = true;
		Out.bCarveStale = !Landscape->IsUpToDate();
	}

	// The final heights and weights, every edit layer merged (an invalid layer guid reads the
	// landscape's own textures), read without touching the GPU copies.
	FLandscapeEditDataInterface Edit(Info, FGuid(), /*bInUploadTextureChangesToGPU*/ false);
	Out.Heights.SetNumZeroed(Out.Width * Out.Height);
	Edit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, Out.Heights.GetData(), Out.Width);

	ULandscapeLayerInfoObject* const Visibility = ALandscapeProxy::VisibilityLayer;
	for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
	{
		ULandscapeLayerInfoObject* LayerInfo = Layer.LayerInfoObj;
		if (!LayerInfo || LayerInfo == Visibility)
		{
			continue;
		}
		// The layer's Phys Material, the landscape's own way of saying what it is made of.
		if (const UPhysicalMaterial* PhysMaterial = LayerInfo->GetPhysicalMaterial())
		{
			Out.LayerPhysMaterials.Add(Layer.GetLayerName(), PhysMaterial);
		}
		TArray<uint8> Weights;
		Weights.SetNumZeroed(Out.Width * Out.Height);
		Edit.GetWeightDataFast(LayerInfo, MinX, MinY, MaxX, MaxY, Weights.GetData(), Out.Width);
		// Layers that exist but are painted nowhere are left out: they would only use up one of the four.
		if (TerrainAnyNonZero(Weights))
		{
			Out.LayerNames.Add(Layer.GetLayerName());
			Out.LayerWeights.Add(MoveTemp(Weights));
		}
	}
	if (Visibility)
	{
		TArray<uint8> Holes;
		Holes.SetNumZeroed(Out.Width * Out.Height);
		Edit.GetWeightDataFast(Visibility, MinX, MinY, MaxX, MaxY, Holes.GetData(), Out.Width);
		Out.bHasHoles = TerrainAnyNonZero(Holes);
		if (Out.bHasHoles)
		{
			for (const uint8 Hole : Holes)
			{
				Out.HoleSamples += Hole != 0 ? 1 : 0;
			}
		}
	}

	// What the landscape's own material says its layers look like, read only when a painted layer has no
	// entry of its own in Mantle Place Settings (those keep their entry).
	bool bBakeAuto = false;
	{
		const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(Landscape->GetWorld());
		bool bNeedsLook = false;
		for (const FName& Name : Out.LayerNames)
		{
			const FMantleTerrainLayerSetting* Entry = Settings ? Settings->FindTerrainLayer(Name) : nullptr;
			bNeedsLook = bNeedsLook || !Entry || !Entry->Material;
		}
		// Nothing painted: unless Place Settings draws it, the material may pick its ground from slope and height, which is baked from the heights.
		const bool bNothingPainted = Out.LayerNames.Num() == 0;
		const bool bSettingsDraw = Settings && Settings->TerrainLayers.Num() > 0 && Settings->TerrainLayers[0].Material;
		if (bNeedsLook || (bNothingPainted && !bSettingsDraw))
		{
			Out.bMaterialRead = FMantleLandscapeMaterialReader::Read(Landscape->GetLandscapeMaterial(), Out.SpacingX, Out.MaterialRead, Out.MaterialProblem);
			for (const FMantleLandscapeLayerLook& Look : Out.MaterialRead.Layers)
			{
				if (Look.bHasAverage)
				{
					Out.LayerColours.Add(Look.Layer, Look.Average);
				}
			}
			// The bake needs the transform, which is set below.
			bBakeAuto = bNothingPainted && Out.bMaterialRead && Out.MaterialRead.AutoChain.bFound;
		}
	}

	// Sample (0, 0) of what was read is landscape sample (MinX, MinY); the entity carries the
	// landscape's rotation, and its scale is already in the spacing and height scale.
	const FTransform ActorTransform = Landscape->GetActorTransform();
	const FVector FirstSample(static_cast<double>(MinX), static_cast<double>(MinY), 0.0);
	Out.Transform = FTransform(ActorTransform.GetRotation(), ActorTransform.TransformPosition(FirstSample), FVector::OneVector);
	if (bBakeAuto)
	{
		BakeAutoChain(Out);
	}
	return true;
}

FMantleTerrainFold FMantleTerrainExport::PlanFold(const FMantleTerrainSource& Source, const TConstArrayView<int32> Candidates, const int32 MaxLayers)
{
	FMantleTerrainFold Fold;
	Fold.SlotOfSource.Init(INDEX_NONE, Source.LayerNames.Num());
	const int32 Max = FMath::Max(MaxLayers, 1);

	// Most painted first (the landscape's order among equals), by the sum of every sample's weight.
	TArray<int32> Ranked;
	TMap<int32, int64> Totals;
	for (const int32 Index : Candidates)
	{
		if (!Source.LayerNames.IsValidIndex(Index) || Ranked.Contains(Index))
		{
			continue;
		}
		int64 Total = 0;
		if (Source.LayerWeights.IsValidIndex(Index))
		{
			for (const uint8 Weight : Source.LayerWeights[Index])
			{
				Total += Weight;
			}
		}
		Totals.Add(Index, Total);
		Ranked.Add(Index);
	}
	Ranked.StableSort([&Totals](const int32 A, const int32 B) { return Totals.FindChecked(A) > Totals.FindChecked(B); });

	for (int32 Rank = 0; Rank < Ranked.Num() && Rank < Max; ++Rank)
	{
		Fold.Kept.Add(Ranked[Rank]);
	}
	Fold.Kept.Sort();
	for (int32 Slot = 0; Slot < Fold.Kept.Num(); ++Slot)
	{
		Fold.SlotOfSource[Fold.Kept[Slot]] = Slot;
	}

	// Each other layer goes where its colour is nearest, else into the most painted layer there is.
	for (int32 Rank = Max; Rank < Ranked.Num(); ++Rank)
	{
		const int32 Index = Ranked[Rank];
		const FLinearColor* Colour = Source.LayerColours.Find(Source.LayerNames[Index]);
		int32 BestSlot = INDEX_NONE;
		double BestDistance = TNumericLimits<double>::Max();
		if (Colour)
		{
			// Most painted kept layer first, so an exact tie goes to it.
			for (int32 KeptRank = 0; KeptRank < Max; ++KeptRank)
			{
				const FLinearColor* KeptColour = Source.LayerColours.Find(Source.LayerNames[Ranked[KeptRank]]);
				if (!KeptColour)
				{
					continue;
				}
				const double Distance = FMath::Square(static_cast<double>(Colour->R - KeptColour->R)) + FMath::Square(static_cast<double>(Colour->G - KeptColour->G))
					+ FMath::Square(static_cast<double>(Colour->B - KeptColour->B));
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BestSlot = Fold.SlotOfSource[Ranked[KeptRank]];
				}
			}
		}
		Fold.SlotOfSource[Index] = BestSlot != INDEX_NONE ? BestSlot : Fold.SlotOfSource[Ranked[0]];
	}
	return Fold;
}

TArray<int32> FMantleTerrainExport::FoldChannels(const TArray<FMantleTerrainLayerChoice>& Layers, const int32 SourceCount)
{
	TArray<int32> Channels;
	Channels.Init(INDEX_NONE, FMath::Max(SourceCount, 0));
	for (int32 Channel = 0; Channel < Layers.Num(); ++Channel)
	{
		if (Channels.IsValidIndex(Layers[Channel].SourceIndex))
		{
			Channels[Layers[Channel].SourceIndex] = Channel;
		}
		for (const int32 Folded : Layers[Channel].FoldedSources)
		{
			if (Channels.IsValidIndex(Folded))
			{
				Channels[Folded] = Channel;
			}
		}
	}
	return Channels;
}

bool FMantleTerrainExport::ChooseLayers(UWorld* World, const FMantleTerrainSource& Source, const FString& Label,
	TArray<FMantleTerrainLayerChoice>& OutLayers, TArray<FMantleValidationIssue>& OutIssues, FMantleCoverage* Coverage)
{
	OutLayers.Reset();
	bool bFailed = false;
	auto Refuse = [&OutIssues, &Label, &bFailed](const FString& Problem, const FString& Remedy)
	{
		bFailed = true;
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label, Problem, Remedy });
	};
	auto Warn = [&OutIssues, &Label](const FString& Problem, const FString& Remedy)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, Label, Problem, Remedy });
	};
	auto Report = [Coverage](const TCHAR* Category, const EMantleCoverageOutcome Outcome, const FString& Name, const FString& Detail)
	{
		if (Coverage)
		{
			Coverage->Add(Category, Outcome, Name, Detail);
		}
	};
	const TCHAR* const Where = TEXT("Mantle Place Settings (Tools > Mantle > Place settings) > Terrain Layers");

	if (Source.bHasHoles)
	{
		// The heightfield format has no hole mask, but the heights under a hole are still there: the published terrain simply draws them. The landscape itself is only read.
		const FString HoleCount = Source.HoleSamples > 0
			? FString::Printf(TEXT("%d of its %d samples"), Source.HoleSamples, Source.Width * Source.Height) : FString(TEXT("part of it"));
		Warn(FString::Printf(TEXT("has holes painted in it (its visibility layer, %s), which Mantle terrain cannot carry yet, so the published terrain is filled across them."), *HoleCount),
			TEXT("It still publishes, and your landscape is not changed. To keep an opening (a cave mouth, a tunnel), model it as a mesh over the filled ground."));
		Report(MantleCoverageCategory::Terrain, EMantleCoverageOutcome::Approximated, Label,
			FString::Printf(TEXT("holes painted in the landscape's visibility layer (%s) are filled: Mantle terrain has no holes, so the ground is drawn across them."), *HoleCount));
	}

	if (Source.WaterBodiesCarving > 0)
	{
		const FString Bodies = FString::Printf(TEXT("%d water bod%s"), Source.WaterBodiesCarving, Source.WaterBodiesCarving == 1 ? TEXT("y") : TEXT("ies"));
		if (Source.bCarveStale)
		{
			Warn(FString::Printf(TEXT("is carved by %s, but its edit layers were not up to date and would not finish updating, so the carve may be missing or old in the published ground."), *Bodies),
				TEXT("Open the level in the editor, let the landscape finish updating (wait for the edit layers progress to end), save, then check again."));
			Report(MantleCoverageCategory::WaterCarve, EMantleCoverageOutcome::Approximated, Label,
				FString::Printf(TEXT("%s carve it, and the landscape's edit layers were out of date, so the carve may be missing or old."), *Bodies));
		}
		else
		{
			Report(MantleCoverageCategory::WaterCarve, EMantleCoverageOutcome::Kept, Label,
				FString::Printf(TEXT("%s carve it; the published ground is the landscape's merged heightmap, carve included%s."), *Bodies,
					Source.bCarveRefreshed ? TEXT(" (its edit layers were rebuilt first)") : TEXT("")));
		}
	}

	const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World);
	if (Source.LayerNames.Num() == 0)
	{
		const FMantleTerrainLayerSetting* First = Settings && Settings->TerrainLayers.Num() > 0 ? &Settings->TerrainLayers[0] : nullptr;
		if (!First || !First->Material)
		{
			// Nothing painted and nothing in Place Settings: the first look the landscape's material states, else plain grey, so the ground is still there.
			const FMantleLandscapeLayerLook* Look = Source.bMaterialRead && Source.MaterialRead.Layers.Num() > 0 ? &Source.MaterialRead.Layers[0] : nullptr;
			FMantleLandscapeLayerLook Plain;
			Plain.Layer = TEXT("Ground");
			FMantleTerrainLayerChoice& Choice = OutLayers.AddDefaulted_GetRef();
			Choice.Layer = Look ? Look->Layer : Plain.Layer;
			Choice.bDerived = Look != nullptr;
			Choice.Material = FMantleLandscapeMaterialReader::MakeLayerMaterial(Look ? *Look : Plain);
			Choice.TilingCm = FMath::Clamp(Look && Look->TilingCm() > 0.0 ? Look->TilingCm() : 400.0, 1.0, 100000.0);
			if (!Choice.Material)
			{
				Refuse(TEXT("has no painted layer and no material to draw it with, so players would see nothing on it."),
					FString::Printf(TEXT("In %s, add an entry with the material the ground should look like."), Where));
			}
			else
			{
				const FString What = Look ? FString::Printf(TEXT("the first look its landscape material states ('%s')"), *Look->Layer.ToString()) : FString(TEXT("plain grey"));
				Warn(FString::Printf(TEXT("has no painted layer and no material in Mantle Place Settings to draw it with, so it is drawn with %s."), *What),
					FString::Printf(TEXT("It still publishes. To draw the ground differently, add an entry in %s with the material it should look like."), Where));
				Report(MantleCoverageCategory::TerrainLayers, EMantleCoverageOutcome::Approximated, Choice.Layer.ToString(),
					FString::Printf(TEXT("nothing is painted on '%s' and Place Settings gives no material: drawn with %s."), *Label, *What));
			}
		}
		else
		{
			FMantleTerrainLayerChoice& Choice = OutLayers.AddDefaulted_GetRef();
			Choice.Layer = First->Layer;
			Choice.Material = First->Material;
			Choice.TilingCm = First->TilingCm;
			Choice.PhysMaterial = Source.LayerPhysMaterials.FindRef(First->Layer);
		}
	}
	else
	{
		// Each painted layer: its Place Settings entry, else what the landscape's material says it looks like,
		// else (a layer the material that was read does not use, such as a grass-removal mask) nothing to draw.
		struct FLayerPlan
		{
			const FMantleTerrainLayerSetting* Entry = nullptr;
			const FMantleLandscapeLayerLook* Look = nullptr;
		};
		TArray<FLayerPlan> Plans;
		Plans.SetNum(Source.LayerNames.Num());
		TArray<int32> Candidates;
		TArray<FString> Undrawn;
		for (int32 Index = 0; Index < Source.LayerNames.Num(); ++Index)
		{
			const FName Name = Source.LayerNames[Index];
			FLayerPlan& Plan = Plans[Index];
			Plan.Entry = Settings ? Settings->FindTerrainLayer(Name) : nullptr;
			if (Plan.Entry && !Plan.Entry->Material)
			{
				Plan.Entry = nullptr;
			}
			Plan.Look = Source.bMaterialRead ? Source.MaterialRead.Find(Name) : nullptr;
			// The baked slope overlay's look is listed under its own name: a painted layer that happens to share it is not the overlay.
			if (Plan.Look && Source.MaterialRead.SlopeOverlay.bFound && Name == Source.MaterialRead.SlopeOverlay.Layer && Source.MaterialRead.Layers.Last().Layer == Name)
			{
				Plan.Look = nullptr;
			}
			if (Plan.Entry || Plan.Look)
			{
				Candidates.Add(Index);
			}
			else if (Source.bMaterialRead)
			{
				Undrawn.Add(Name.ToString());
			}
			else
			{
				Refuse(FString::Printf(TEXT("has paint layer '%s', which has no material to draw it with in Mantle. %s"), *Name.ToString(), *Source.MaterialProblem).TrimEnd(),
					FString::Printf(TEXT("In %s, add '%s' with the material it should look like."), Where, *Name.ToString()));
			}
		}
		if (Candidates.Num() == 0 && !bFailed)
		{
			Refuse(FString::Printf(TEXT("has paint layers (%s) that its landscape material does not use and Mantle Place Settings gives no entry, so players would see nothing on it."),
					*FString::Join(Undrawn, TEXT(", "))),
				FString::Printf(TEXT("In %s, add an entry with the material each layer should look like."), Where));
		}

		if (!bFailed)
		{
			// The landscape material's slope overlay is baked into a layer of its own, so it takes one of the slots.
			const FMantleLandscapeLayerLook* OverlayLook = Source.bMaterialRead && Source.MaterialRead.SlopeOverlay.bFound
				? Source.MaterialRead.Find(Source.MaterialRead.SlopeOverlay.Layer) : nullptr;
			const int32 MaxPainted = FMath::Max(FMantlePlaceLimits::MaxTerrainLayers - (OverlayLook ? 1 : 0), 1);
			const FMantleTerrainFold Fold = PlanFold(Source, Candidates, MaxPainted);
			bool bAnyDerived = false;
			for (const int32 Index : Fold.Kept)
			{
				const FName Name = Source.LayerNames[Index];
				const FLayerPlan& Plan = Plans[Index];
				FMantleTerrainLayerChoice& Choice = OutLayers.AddDefaulted_GetRef();
				Choice.Layer = Name;
				Choice.SourceIndex = Index;
				Choice.PhysMaterial = Source.LayerPhysMaterials.FindRef(Name);
				if (Plan.Entry)
				{
					Choice.Material = Plan.Entry->Material;
					Choice.TilingCm = Plan.Entry->TilingCm;
					continue;
				}
				// A look read from the landscape's own material.
				bAnyDerived = true;
				const FMantleLandscapeLayerLook& Look = *Plan.Look;
				Choice.bDerived = true;
				Choice.Material = FMantleLandscapeMaterialReader::MakeLayerMaterial(Look);
				Choice.TilingCm = FMath::Clamp(Look.TilingCm() > 0.0 ? Look.TilingCm() : 400.0, 1.0, 100000.0);
				if (!Choice.Material)
				{
					Refuse(FString::Printf(TEXT("has paint layer '%s', whose look from its landscape material could not be made into a material."), *Name.ToString()),
						FString::Printf(TEXT("In %s, add '%s' with the material it should look like."), Where, *Name.ToString()));
					continue;
				}
				TArray<FString> Notes = Look.Approximations;
				if (Look.bUnreadable)
				{
					Notes.Add(TEXT("its look is not a texture or a colour the material states, so it is drawn plain grey."));
				}
				for (const FString& Note : Notes)
				{
					// A layer baked from the material's slope and height rules says so in the coverage report; it is not a warning.
					if (Note == FMantleLandscapeAutoChain::BakedNote())
					{
						continue;
					}
					Warn(FString::Printf(TEXT("has paint layer '%s', drawn from its landscape material, and %s"), *Name.ToString(), *Note),
						TEXT("It still publishes. To draw the layer differently, give it an entry in Mantle Place Settings > Terrain Layers."));
				}
			}

			// More painted layers than Mantle publishes: the rest are folded into the nearest kept one.
			TArray<FString> Folds;
			for (int32 Index = 0; Index < Source.LayerNames.Num(); ++Index)
			{
				if (Fold.IsFolded(Index))
				{
					const int32 Slot = Fold.SlotOfSource[Index];
					if (OutLayers.IsValidIndex(Slot))
					{
						OutLayers[Slot].FoldedSources.Add(Index);
						Folds.Add(FString::Printf(TEXT("'%s' into '%s'"), *Source.LayerNames[Index].ToString(), *OutLayers[Slot].Layer.ToString()));
					}
				}
			}
			if (Folds.Num() > 0)
			{
				Warn(FString::Printf(TEXT("has %d painted layers; Mantle publishes at most %d of them (a slope overlay, where there is one, takes a place too), so it keeps the %d most painted and folds the others into the nearest by colour: %s."),
						Candidates.Num(), MaxPainted, MaxPainted, *FString::Join(Folds, TEXT(", "))),
					TEXT("It still publishes, with those layers painted as the layers they were folded into. Nothing to do; to keep a different layer, paint more of it."));
			}
			// The slope overlay, last: a layer of its own whose weight is baked from the heightfield.
			if (OverlayLook)
			{
				bAnyDerived = true;
				FMantleTerrainLayerChoice& Overlay = OutLayers.AddDefaulted_GetRef();
				Overlay.Layer = OverlayLook->Layer;
				Overlay.bDerived = true;
				Overlay.bSlopeOverlay = true;
				Overlay.Material = FMantleLandscapeMaterialReader::MakeLayerMaterial(*OverlayLook);
				Overlay.TilingCm = FMath::Clamp(OverlayLook->TilingCm() > 0.0 ? OverlayLook->TilingCm() : 400.0, 1.0, 100000.0);
				if (!Overlay.Material)
				{
					Refuse(TEXT("has a slope overlay in its landscape material, whose look could not be made into a material."),
						TEXT("This is a Mantle bug rather than a problem with your level. Please report it."));
				}
				// Its baked note is the coverage report's to say (Approximated); what else the material does to it is warned.
				for (const FString& Note : OverlayLook->Approximations)
				{
					if (Note != FMantleLandscapeSlopeOverlay::BakedNote())
					{
						Warn(FString::Printf(TEXT("has a slope overlay in its landscape material, drawn as the layer '%s', and %s"), *OverlayLook->Layer.ToString(), *Note),
							TEXT("It still publishes."));
					}
				}
			}
			if (Undrawn.Num() > 0)
			{
				Warn(FString::Printf(TEXT("has painted layers its landscape material does not use (%s), so they are not drawn and their paint is left out."), *FString::Join(Undrawn, TEXT(", "))),
					FString::Printf(TEXT("They still publish without them. To draw one, give it an entry in %s."), Where));
			}

			// What the landscape material does that a flat tiled layer cannot keep, when its looks are what is drawn.
			if (bAnyDerived)
			{
				for (const FString& Approximation : Source.MaterialRead.Approximations)
				{
					Warn(FString::Printf(TEXT("has a landscape material ('%s') that does something Mantle does not keep: %s"), *Source.MaterialRead.MaterialName, *Approximation),
						TEXT("It still publishes, with each layer drawn flat. If it matters, bake the effect into the layer's textures, or give the layers entries in Mantle Place Settings > Terrain Layers."));
					Report(MantleCoverageCategory::LandscapeMaterial, EMantleCoverageOutcome::Approximated, Source.MaterialRead.MaterialName, Approximation);
				}
				if (Source.MaterialRead.Approximations.Num() == 0)
				{
					Report(MantleCoverageCategory::LandscapeMaterial, EMantleCoverageOutcome::Kept, Source.MaterialRead.MaterialName, TEXT("every layer's textures and tiling read from it"));
				}
			}
		}
	}

	if (bFailed)
	{
		OutLayers.Reset();
		return false;
	}
	return true;
}

FMantleHeightfield FMantleTerrainExport::MakeHeightfield(const FMantleTerrainSource& Source, const int32 MaxSamples)
{
	using namespace MantleTerrainExportPrivate;

	FMantleHeightfield Out;
	if (Source.Width < 2 || Source.Height < 2 || Source.Heights.Num() != Source.Width * Source.Height || MaxSamples < 2)
	{
		return Out;
	}
	Out.Width = FMath::Min(Source.Width, MaxSamples);
	Out.Height = FMath::Min(Source.Height, MaxSamples);
	// Resampling keeps the terrain's size: fewer samples, farther apart.
	Out.SpacingX = static_cast<float>(Source.SpacingX * (Source.Width - 1) / (Out.Width - 1));
	Out.SpacingY = static_cast<float>(Source.SpacingY * (Source.Height - 1) / (Out.Height - 1));
	Out.HeightScale = static_cast<float>(Source.HeightScale);
	Out.HeightOffset = static_cast<float>(Source.HeightOffset);
	if (Out.Width == Source.Width && Out.Height == Source.Height)
	{
		Out.Heights = Source.Heights;
		return Out;
	}
	Out.Heights.SetNumUninitialized(Out.Width * Out.Height);
	const double StepX = static_cast<double>(Source.Width - 1) / (Out.Width - 1);
	const double StepY = static_cast<double>(Source.Height - 1) / (Out.Height - 1);
	for (int32 Y = 0; Y < Out.Height; ++Y)
	{
		for (int32 X = 0; X < Out.Width; ++X)
		{
			const double Value = MantleTerrainSampling::Bilinear(Source.Heights, Source.Width, Source.Height, X * StepX, Y * StepY);
			Out.Heights[Y * Out.Width + X] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt32(Value), 0, 65535));
		}
	}
	return Out;
}

FIntPoint FMantleTerrainExport::WeightmapSize(const int32 Width, const int32 Height, const int32 MaxSize)
{
	return FIntPoint(FMath::Clamp(Width, 1, FMath::Max(MaxSize, 1)), FMath::Clamp(Height, 1, FMath::Max(MaxSize, 1)));
}

int32 FMantleTerrainExport::WeightmapCount(const int32 LayerCount)
{
	return LayerCount > MantleTerrainExportPrivate::TerrainChannelsPerMap ? 2 : (LayerCount > 1 ? 1 : 0);
}

TArray<uint8> FMantleTerrainExport::BakeSlopeMask(const FMantleTerrainSource& Source, const FMantleLandscapeSlopeOverlay& Overlay)
{
	TArray<uint8> Mask;
	if (!Overlay.bFound || Source.Width < 2 || Source.Height < 2 || Source.Heights.Num() != Source.Width * Source.Height)
	{
		return Mask;
	}
	Mask.SetNumUninitialized(Source.Width * Source.Height);
	for (int32 Y = 0; Y < Source.Height; ++Y)
	{
		for (int32 X = 0; X < Source.Width; ++X)
		{
			double SlopeX = 0.0;
			double SlopeY = 0.0;
			MantleTerrainExportPrivate::TerrainSlopeAt(Source, X, Y, SlopeX, SlopeY);
			// The normal (-SlopeX, -SlopeY, 1) normalised: its Z is what the material's mask reads.
			const double NormalZ = 1.0 / FMath::Sqrt(1.0 + SlopeX * SlopeX + SlopeY * SlopeY);
			Mask[Y * Source.Width + X] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Overlay.WeightAt(NormalZ) * 255.0f), 0, 255));
		}
	}
	return Mask;
}

TArray<TArray<uint8>> FMantleTerrainExport::BakeAutoWeights(const FMantleTerrainSource& Source)
{
	using namespace MantleTerrainExportPrivate;

	TArray<TArray<uint8>> Weights;
	const FMantleLandscapeAutoChain& Chain = Source.MaterialRead.AutoChain;
	const int32 Layers = Chain.LayerNames.Num();
	if (!Chain.bFound || !Chain.Program.IsValid() || Layers < 2 || Layers > FMantleLandscapeAutoChain::MaxLayers || Chain.Program->NumOutputs() != Layers - 1
		|| Source.Width < 2 || Source.Height < 2 || Source.Heights.Num() != Source.Width * Source.Height)
	{
		return Weights;
	}
	Weights.SetNum(Layers);
	TArray<uint8*, TInlineAllocator<FMantleLandscapeAutoChain::MaxLayers>> Out;
	for (TArray<uint8>& Layer : Weights)
	{
		Layer.SetNumUninitialized(Source.Width * Source.Height);
		Out.Add(Layer.GetData());
	}

	const FMantleSurfaceProgram& Program = *Chain.Program;
	ParallelFor(Source.Height, [&Source, &Program, &Out, Layers](const int32 Y)
	{
		double Masks[FMantleLandscapeAutoChain::MaxLayers] = {};
		double Share[FMantleLandscapeAutoChain::MaxLayers] = {};
		for (int32 X = 0; X < Source.Width; ++X)
		{
			Program.Evaluate(TerrainSurfaceAt(Source, X, Y), Masks);

			// The last blend is on top: its mask is its layer's weight, and what it leaves goes to the ones under it.
			double Rest = 1.0;
			for (int32 Layer = Layers - 1; Layer >= 1; --Layer)
			{
				const double Mask = FMath::IsFinite(Masks[Layer - 1]) ? FMath::Clamp(Masks[Layer - 1], 0.0, 1.0) : 0.0;
				Share[Layer] = Mask * Rest;
				Rest *= 1.0 - Mask;
			}
			Share[0] = Rest;

			// Normalised to 255, the rounding remainder given to the heaviest layer so the sum is exact.
			int32 Channels[FMantleLandscapeAutoChain::MaxLayers] = {};
			int32 Total = 0;
			int32 Heaviest = 0;
			for (int32 Layer = 0; Layer < Layers; ++Layer)
			{
				Channels[Layer] = FMath::Clamp(FMath::RoundToInt32(Share[Layer] * 255.0), 0, 255);
				Total += Channels[Layer];
				Heaviest = Share[Layer] > Share[Heaviest] ? Layer : Heaviest;
			}
			Channels[Heaviest] = FMath::Clamp(Channels[Heaviest] + (255 - Total), 0, 255);
			for (int32 Layer = 0; Layer < Layers; ++Layer)
			{
				Out[Layer][Y * Source.Width + X] = static_cast<uint8>(Channels[Layer]);
			}
		}
	});
	return Weights;
}

bool FMantleTerrainExport::BakeAutoChain(FMantleTerrainSource& Source)
{
	TArray<TArray<uint8>> Weights = BakeAutoWeights(Source);
	bool bAdded = false;
	for (int32 Layer = 0; Layer < Weights.Num(); ++Layer)
	{
		// A layer that weighs nothing anywhere would only use up one of the places.
		if (MantleTerrainExportPrivate::TerrainAnyNonZero(Weights[Layer]))
		{
			Source.LayerNames.Add(Source.MaterialRead.AutoChain.LayerNames[Layer]);
			Source.LayerWeights.Add(MoveTemp(Weights[Layer]));
			bAdded = true;
		}
	}
	return bAdded;
}

TArray<FColor> FMantleTerrainExport::MakeWeightmap(const FMantleTerrainSource& Source, TConstArrayView<int32> SourceLayers, const FIntPoint Size,
	const TConstArrayView<int32> ChannelOfSource)
{
	using namespace MantleTerrainExportPrivate;

	// Which source layers add to which channel: the four named, then any folded into one of them.
	FTerrainWeightPlan Plan;
	Plan.Count = FMath::Min(SourceLayers.Num(), TerrainChannelsPerMap);
	for (int32 Layer = 0; Layer < Plan.Count; ++Layer)
	{
		if (TerrainHasWeights(Source, SourceLayers[Layer]))
		{
			Plan.Contributions.Emplace(SourceLayers[Layer], Layer);
		}
	}
	for (int32 SourceIndex = 0; SourceIndex < ChannelOfSource.Num(); ++SourceIndex)
	{
		const int32 Channel = ChannelOfSource[SourceIndex];
		if (!SourceLayers.Contains(SourceIndex) && Channel >= 0 && Channel < Plan.Count && TerrainHasWeights(Source, SourceIndex))
		{
			Plan.Contributions.Emplace(SourceIndex, Channel);
		}
	}
	TArray<FColor> First;
	TArray<FColor> Second;
	BuildTerrainWeightTexels(Source, Plan, TArray<uint8>(), Size, First, Second);
	return First;
}

void FMantleTerrainExport::MakeWeightmaps(const FMantleTerrainSource& Source, const TArray<FMantleTerrainLayerChoice>& Layers, const FIntPoint Size,
	TArray<FColor>& OutFirst, TArray<FColor>& OutSecond)
{
	using namespace MantleTerrainExportPrivate;

	FTerrainWeightPlan Plan;
	Plan.Count = FMath::Min(Layers.Num(), FMantlePlaceLimits::MaxTerrainLayers);
	for (int32 Channel = 0; Channel < Plan.Count; ++Channel)
	{
		const FMantleTerrainLayerChoice& Layer = Layers[Channel];
		if (Layer.bSlopeOverlay)
		{
			Plan.SlopeChannel = Channel;
			continue;
		}
		if (TerrainHasWeights(Source, Layer.SourceIndex))
		{
			Plan.Contributions.Emplace(Layer.SourceIndex, Channel);
		}
		// Layers folded into this one paint into its channel.
		for (const int32 Folded : Layer.FoldedSources)
		{
			if (TerrainHasWeights(Source, Folded))
			{
				Plan.Contributions.Emplace(Folded, Channel);
			}
		}
	}
	const TArray<uint8> SlopeMask = Plan.SlopeChannel != INDEX_NONE ? BakeSlopeMask(Source, Source.MaterialRead.SlopeOverlay) : TArray<uint8>();
	BuildTerrainWeightTexels(Source, Plan, SlopeMask, Size, OutFirst, OutSecond);
}

bool FMantleTerrainExport::EncodeWeightmaps(const FMantleTerrainSource& Source, const TArray<FMantleTerrainLayerChoice>& Layers, const int32 MaxSize, const int32 MaxBytes,
	FIntPoint& OutSize, TArray<uint8>& OutFirst, TArray<uint8>& OutSecond)
{
	FIntPoint Size = WeightmapSize(Source.Width, Source.Height, MaxSize);
	TArray<FColor> First;
	TArray<FColor> Second;
	TArray<uint8> FirstPng;
	TArray<uint8> SecondPng;
	for (;;)
	{
		MakeWeightmaps(Source, Layers, Size, First, Second);
		FirstPng = FMantlePng::EncodeRGBA8(Size.X, Size.Y, First);
		SecondPng = Second.Num() > 0 ? FMantlePng::EncodeRGBA8(Size.X, Size.Y, Second) : TArray<uint8>();
		const bool bFits = FirstPng.Num() <= MaxBytes && SecondPng.Num() <= MaxBytes;
		if (bFits || (Size.X <= 64 && Size.Y <= 64))
		{
			break;
		}
		Size = FIntPoint(FMath::Max(Size.X / 2, 1), FMath::Max(Size.Y / 2, 1));
	}
	if (FirstPng.IsEmpty() || FirstPng.Num() > MaxBytes || (Second.Num() > 0 && (SecondPng.IsEmpty() || SecondPng.Num() > MaxBytes)))
	{
		return false;
	}
	OutSize = Size;
	OutFirst = MoveTemp(FirstPng);
	OutSecond = MoveTemp(SecondPng);

	// What each channel holds, for whoever checks a landscape's layers after an export.
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_WEIGHTMAPS %d layer(s) in %d map(s), %d x %d texels, %d + %d bytes"), Layers.Num(), OutSecond.IsEmpty() ? 1 : 2, Size.X, Size.Y,
		OutFirst.Num(), OutSecond.Num());
	for (int32 Channel = 0; Channel < Layers.Num(); ++Channel)
	{
		const FMantleTerrainLayerChoice& Layer = Layers[Channel];
		TArray<FString> Folded;
		for (const int32 Index : Layer.FoldedSources)
		{
			Folded.Add(Source.LayerNames.IsValidIndex(Index) ? Source.LayerNames[Index].ToString() : FString::FromInt(Index));
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_LAYER map%d.%c %s material=%s tiling=%g cm%s%s%s"), Channel / 4 + 1, TEXT("rgba")[Channel % 4], *Layer.Layer.ToString(),
			Layer.Material ? *Layer.Material->GetName() : TEXT("none"), Layer.TilingCm, Layer.bSlopeOverlay ? TEXT(" slope-overlay-baked") : TEXT(""),
			Folded.Num() > 0 ? TEXT(" folded: ") : TEXT(""), *FString::Join(Folded, TEXT(", ")));
	}
	return true;
}

void FMantleTerrainExport::CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantleTerrainExportPrivate;

	const TArray<AActor*> Landscapes = CollectLandscapes(World);
	for (int32 Index = 0; Index < Landscapes.Num(); ++Index)
	{
		const AActor* Landscape = Landscapes[Index];
		const FString Label = TerrainLabelOf(Landscape);
		if (Index >= FMantlePlaceLimits::MaxTerrainsPerPlace)
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label,
				FString::Printf(TEXT("is landscape number %d; a place publishes at most %d."), Index + 1, FMantlePlaceLimits::MaxTerrainsPerPlace),
				TEXT("Merge the landscapes into one, or remove the extra ones, then publish again.") });
			continue;
		}
		FMantleTerrainSource Source;
		FString Problem;
		if (!ReadLandscape(Landscape, Source, Problem))
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label, Problem,
				TEXT("Fix it in the landscape's Details panel, then check again.") });
			continue;
		}
		TArray<FMantleTerrainLayerChoice> Layers;
		ChooseLayers(World, Source, Label, Layers, OutIssues);
		if (Source.Width > FMantleHeightfieldReader::MaxSize || Source.Height > FMantleHeightfieldReader::MaxSize)
		{
			const FMantleHeightfield Resampled = MakeHeightfield(Source);
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, Label,
				FString::Printf(TEXT("is %d x %d samples; Mantle publishes at most %d on a side, so players get %d x %d, %.0f x %.0f cm apart."),
					Source.Width, Source.Height, FMantleHeightfieldReader::MaxSize, Resampled.Width, Resampled.Height,
					Resampled.SpacingX, Resampled.SpacingY),
				TEXT("Nothing to do. For every sample, make the landscape smaller or split the world into more than one place.") });
		}
	}
}
