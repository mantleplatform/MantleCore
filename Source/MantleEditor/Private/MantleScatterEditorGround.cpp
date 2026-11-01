// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScatterEditorGround.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"
#include "MantleTerrainExport.h"
#include "MantleTerrainSurface.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "LandscapeProxy.h"
#include "Containers/Ticker.h"
#include "Misc/TransactionObjectEvent.h"
#include "UObject/UObjectGlobals.h"

namespace MantleScatterEditorGroundPrivate
{
	/** Seconds a landscape must stay unchanged before its ground is read again: a sculpt stroke modifies it every frame. */
	constexpr double SettleSeconds = 1.0;

	/** The ground the volumes were last given, and the generation they were given it at. */
	TArray<FMantleScatterTerrainSurface> Surfaces;
	int32 Generation = 0;
	bool bDirty = true;
	double DirtySince = 0.0;
	TMap<TWeakObjectPtr<AMantleScatterVolume>, int32> FedAt;
	TWeakObjectPtr<UWorld> FedWorld;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle ModifiedHandle;
	FDelegateHandle TransactedHandle;
	FDelegateHandle PropertyHandle;
	FDelegateHandle UndoHandle;
	FDelegateHandle MapOpenedHandle;

	void MarkDirty()
	{
		bDirty = true;
		DirtySince = FPlatformTime::Seconds();
	}

	/** A landscape, or something it owns (a component, a heightmap or weightmap texture): what a sculpt or paint stroke modifies. */
	bool IsLandscapePart(const UObject* Object)
	{
		for (const UObject* Outer = Object; Outer; Outer = Outer->GetOuter())
		{
			if (Outer->IsA<ALandscapeProxy>())
			{
				return true;
			}
		}
		return false;
	}

	void OnObjectChanged(UObject* Object)
	{
		if (IsLandscapePart(Object))
		{
			MarkDirty();
		}
	}

	UWorld* EditorWorld()
	{
		return GEditor && GEditor->GetEditorWorldContext().World() ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	bool Tick(float)
	{
		UWorld* World = EditorWorld();
		if (!World || (GEditor && GEditor->PlayWorld))
		{
			return true;
		}
		if (FedWorld.Get() != World)
		{
			FedWorld = World;
			FedAt.Reset();
			MarkDirty();
		}
		TArray<AMantleScatterVolume*> Waiting;
		for (TActorIterator<AMantleScatterVolume> It(World); It; ++It)
		{
			if (IsValid(*It) && It->Placement == EMantleScatterPlacement::Terrain)
			{
				const int32* Fed = FedAt.Find(*It);
				if (!Fed || *Fed != Generation || bDirty)
				{
					Waiting.Add(*It);
				}
			}
		}
		if (Waiting.Num() == 0)
		{
			return true;
		}
		if (bDirty)
		{
			if (FPlatformTime::Seconds() - DirtySince < SettleSeconds)
			{
				return true;
			}
			Surfaces = FMantleScatterEditorGround::SurfacesFor(World);
			++Generation;
			bDirty = false;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_SCATTER editor ground: %d landscape surface(s), generation %d"), Surfaces.Num(), Generation);
		}
		for (AMantleScatterVolume* Volume : Waiting)
		{
			const int32* Fed = FedAt.Find(Volume);
			if (!Fed || *Fed != Generation)
			{
				Volume->SetTerrainSurfaces(Surfaces);
				FedAt.Add(Volume, Generation);
			}
		}
		return true;
	}
}

void FMantleScatterEditorGround::Startup()
{
	using namespace MantleScatterEditorGroundPrivate;
	if (IsRunningCommandlet())
	{
		return; // no preview to feed; a commandlet that wants one calls FeedVolumes
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.25f);
	ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddStatic(&OnObjectChanged);
	TransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddLambda(
		[](UObject* Object, const FTransactionObjectEvent&) { OnObjectChanged(Object); });
	PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
		[](UObject* Object, FPropertyChangedEvent&) { OnObjectChanged(Object); });
	UndoHandle = FEditorDelegates::PostUndoRedo.AddStatic(&MarkDirty);
	MapOpenedHandle = FEditorDelegates::OnMapOpened.AddLambda([](const FString&, bool) { FedAt.Reset(); MarkDirty(); });
}

void FMantleScatterEditorGround::Shutdown()
{
	using namespace MantleScatterEditorGroundPrivate;
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
	FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactedHandle);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
	FEditorDelegates::PostUndoRedo.Remove(UndoHandle);
	FEditorDelegates::OnMapOpened.Remove(MapOpenedHandle);
	Surfaces.Reset();
	FedAt.Reset();
}

FTransform FMantleScatterEditorGround::AsPublished(const FTransform& Transform)
{
	return FTransform(FRotator(Transform.GetRotation().Rotator()), Transform.GetLocation(), Transform.GetScale3D());
}

FMantleScatterTerrainSurface FMantleScatterEditorGround::SurfaceFor(const FMantleTerrainSource& Source, UWorld* World, const FString& Label)
{
	FMantleScatterTerrainSurface Out;
	Out.TerrainToWorld = AsPublished(Source.Transform);
	// Through the MHF1 file, as the client gets it: the floats it stores are the floats it reads.
	const TArray<uint8> Bytes = FMantleHeightfieldReader::Write(FMantleTerrainExport::MakeHeightfield(Source));
	TSharedRef<FMantleHeightfield> Field = MakeShared<FMantleHeightfield>();
	if (Bytes.IsEmpty() || !FMantleHeightfieldReader::Read(Bytes, *Field))
	{
		return Out;
	}
	Out.Field = Field;
	// The hole mask at the size the converter uploads it (lossless), read the way the place builder reads it.
	if (FMantleTerrainExport::HasHoles(Source))
	{
		const int32 MaxSize = FMath::Max(FMantlePlaceAssetLimits().MaxTextureSize, 1);
		const FIntPoint Size(FMath::Clamp(Field->Width, 1, MaxSize), FMath::Clamp(Field->Height, 1, MaxSize));
		const TArray<FColor> Texels = FMantleTerrainExport::MakeHoleMask(Source, Size);
		FMantleTerrainWeights Mask;
		if (FMantleTerrainWeights::FromPixels(Size.X, Size.Y, Texels, Mask, /*bKeepPixels*/ true))
		{
			Out.Holes = FMantleTerrainHoleGrid::FromMask(Mask, Field->Width, Field->Height);
		}
	}
	// The paint layers (#187) the place carries: one layer has no weightmap, more have one (a second past four), at the size the
	// converter uploads them. The PNG is lossless, so the pixels MakeWeightmaps gives at that size are the ones a client decodes.
	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> LayerIssues;
	if (World && FMantleTerrainExport::ChooseLayers(World, Source, Label, Layers, LayerIssues))
	{
		Out.LayerCount = Layers.Num();
		Out.bWeightmap = Layers.Num() > 1;
		const FMantlePlaceAssetLimits Limits;
		FIntPoint Size;
		TArray<uint8> Png;
		TArray<uint8> Png2;
		if (Out.bWeightmap && FMantleTerrainExport::EncodeWeightmaps(Source, Layers, Limits.MaxTextureSize, Limits.MaxTextureBytes, Size, Png, Png2))
		{
			TArray<FColor> First;
			TArray<FColor> Second;
			FMantleTerrainExport::MakeWeightmaps(Source, Layers, Size, First, Second);
			TSharedRef<FMantleTerrainWeights> Weights = MakeShared<FMantleTerrainWeights>();
			if (FMantleTerrainWeights::FromPixels(Size.X, Size.Y, First, *Weights, /*bKeepPixels*/ true))
			{
				Out.Weights = Weights;
			}
			TSharedRef<FMantleTerrainWeights> Weights2 = MakeShared<FMantleTerrainWeights>();
			if (!Second.IsEmpty() && FMantleTerrainWeights::FromPixels(Size.X, Size.Y, Second, *Weights2, /*bKeepPixels*/ true))
			{
				Out.Weights2 = Weights2;
			}
		}
	}
	return Out;
}

TArray<FMantleScatterTerrainSurface> FMantleScatterEditorGround::SurfacesFor(UWorld* World)
{
	TArray<FMantleScatterTerrainSurface> Out;
	for (const AActor* Landscape : FMantleTerrainExport::TerrainLandscapes(World))
	{
		FMantleTerrainSource Source;
		FString Problem;
		if (!FMantleTerrainExport::ReadLandscape(Landscape, Source, Problem))
		{
			UE_LOG(LogMantle, Verbose, TEXT("MANTLE_SCATTER editor ground: %s is left out: %s"), *Landscape->GetName(), *Problem);
			continue;
		}
		FMantleScatterTerrainSurface Surface = SurfaceFor(Source, World, Landscape->GetActorLabel());
		if (Surface.Field.IsValid())
		{
			Out.Add(MoveTemp(Surface));
		}
	}
	return Out;
}

void FMantleScatterEditorGround::FeedVolumes(UWorld* World)
{
	if (!World)
	{
		return;
	}
	TArray<FMantleScatterTerrainSurface> Surfaces;
	bool bRead = false;
	for (TActorIterator<AMantleScatterVolume> It(World); It; ++It)
	{
		if (IsValid(*It) && It->Placement == EMantleScatterPlacement::Terrain)
		{
			if (!bRead)
			{
				Surfaces = SurfacesFor(World);
				bRead = true;
			}
			It->SetTerrainSurfaces(Surfaces);
		}
	}
}
