// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMinimap.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceValidator.h"
#include "MantleRuntime.h"
#include "MantleSplineMeshBake.h"
#include "MantleTerrainExport.h"
#include "MantleThumbnailChoice.h"
#include "MantleWaterBodyExport.h"

#include "Async/ParallelFor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"

#include <atomic>

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMinimapPrivate
{
	/** A piece bigger than this (half-extent, cm) is a sky or a backdrop: never drawn, never part of the area. */
	constexpr double MaxPieceHalfExtentCm = 500000.0;

	/** Points drawn along each span of a water body's spline. */
	constexpr int32 WaterSplineSteps = 8;

	bool Collides(const UPrimitiveComponent* Component)
	{
		return Component->IsCollisionEnabled() && Component->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block;
	}

	/** An actor whose static meshes the map draws: placed content Export publishes as meshes (landscapes and Water Bodies are drawn their own way). */
	bool IsDrawnActor(const AActor* Actor)
	{
		return IsValid(Actor) && !Actor->IsHidden() && !Actor->IsEditorOnly() && !Actor->IsA<ALandscapeProxy>()
			&& !FMantlePlaceExporter::IsEngineInfrastructure(Actor) && !FMantlePlaceExporter::IsProvidedByMantle(Actor)
			&& !FMantleWaterBodyExport::IsWaterBodyActor(Actor) && !FMantleWaterBodyExport::IsWaterInfrastructure(Actor);
	}

	/** A mesh a player sees; spline meshes are drawn from their bake. */
	bool IsDrawnMesh(const UStaticMeshComponent* Component)
	{
		if (!Component || !Component->IsRegistered() || !Component->IsVisible() || Component->bHiddenInGame || Component->IsEditorOnly()
			|| !Component->GetStaticMesh() || Component->IsA<USplineMeshComponent>())
		{
			return false;
		}
		const FBox Box = Component->Bounds.GetBox();
		return Box.IsValid != 0 && Box.GetExtent().GetMax() <= MaxPieceHalfExtentCm;
	}

	/** One mesh's render LOD 0 (a Nanite mesh's fallback), read once however often it is placed. */
	struct FMeshTriangles
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		/** Per section: first index, triangle count, material slot. */
		TArray<FIntVector3> Sections;
		FBox Bounds = FBox(ForceInit);
	};

	bool ReadMesh(const UStaticMesh* Mesh, FMeshTriangles& Out)
	{
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData())
		{
			return false;
		}
		Out.Positions.SetNumUninitialized(Positions.GetNumVertices());
		for (uint32 Vertex = 0; Vertex < Positions.GetNumVertices(); ++Vertex)
		{
			Out.Positions[Vertex] = Positions.VertexPosition(Vertex);
			Out.Bounds += FVector(Out.Positions[Vertex]);
		}
		LOD.IndexBuffer.GetCopy(Out.Indices);
		if (Out.Indices.Num() == 0)
		{
			return false;
		}
		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			Out.Sections.Add(FIntVector3(static_cast<int32>(Section.FirstIndex), static_cast<int32>(Section.NumTriangles), Section.MaterialIndex));
		}
		return true;
	}

	/** The first of Names that says what a surface is, else Fallback. */
	EMantleMinimapSurface SurfaceOfNames(std::initializer_list<FString> Names, EMantleMinimapSurface Fallback)
	{
		for (const FString& Name : Names)
		{
			const EMantleMinimapSurface Surface = FMantleMinimapCanvas::Classify(Name);
			if (Surface != EMantleMinimapSurface::None)
			{
				return Surface;
			}
		}
		return Fallback;
	}

	/** One mesh drawn with a surface per section, placed once or many times. */
	struct FPiece
	{
		int32 Mesh = INDEX_NONE;
		TArray<EMantleMinimapSurface> Surfaces;
	};

	struct FDraw
	{
		int32 Piece = INDEX_NONE;
		FTransform Transform;
	};

	/** Meshes read once each, pieces and their placements. */
	struct FMeshDraws
	{
		TArray<FMeshTriangles> Meshes;
		TMap<const UStaticMesh*, int32> MeshIndex;
		TArray<FPiece> Pieces;
		TArray<FDraw> Draws;
		int32 Unreadable = 0;

		int32 MeshOf(const UStaticMesh* Mesh)
		{
			if (const int32* Found = MeshIndex.Find(Mesh))
			{
				return *Found;
			}
			FMeshTriangles Read;
			const int32 Index = ReadMesh(Mesh, Read) ? Meshes.Add(MoveTemp(Read)) : INDEX_NONE;
			MeshIndex.Add(Mesh, Index);
			Unreadable += Index == INDEX_NONE ? 1 : 0;
			return Index;
		}

		/** Placements whose box misses Area are left out. */
		void Place(int32 Piece, const FTransform& Transform, const FBox2D& Area)
		{
			const FBox Box = Meshes[Pieces[Piece].Mesh].Bounds.TransformBy(Transform);
			if (Box.IsValid != 0 && Area.Intersect(FBox2D(FVector2D(Box.Min), FVector2D(Box.Max))))
			{
				Draws.Add(FDraw{ Piece, Transform });
			}
		}

		int64 DrawAll(FMantleMinimapCanvas& Canvas) const
		{
			std::atomic<int64> Triangles(0);
			ParallelFor(Draws.Num(), [this, &Canvas, &Triangles](int32 DrawIndex)
			{
				const FDraw& Draw = Draws[DrawIndex];
				const FPiece& Piece = Pieces[Draw.Piece];
				const FMeshTriangles& Mesh = Meshes[Piece.Mesh];
				TArray<FVector> Corners;
				Corners.SetNumUninitialized(Mesh.Positions.Num());
				for (int32 Vertex = 0; Vertex < Mesh.Positions.Num(); ++Vertex)
				{
					Corners[Vertex] = Draw.Transform.TransformPosition(FVector(Mesh.Positions[Vertex]));
				}
				const uint32 Count = static_cast<uint32>(Corners.Num());
				int64 Drawn = 0;
				for (int32 SectionIndex = 0; SectionIndex < Mesh.Sections.Num() && SectionIndex < Piece.Surfaces.Num(); ++SectionIndex)
				{
					const FIntVector3& Section = Mesh.Sections[SectionIndex];
					for (int32 Triangle = 0; Triangle < Section.Y; ++Triangle)
					{
						const int32 First = Section.X + Triangle * 3;
						if (First + 2 >= Mesh.Indices.Num())
						{
							break;
						}
						const uint32 I0 = Mesh.Indices[First], I1 = Mesh.Indices[First + 1], I2 = Mesh.Indices[First + 2];
						if (I0 < Count && I1 < Count && I2 < Count)
						{
							Canvas.DrawTriangle(Corners[I0], Corners[I1], Corners[I2], Piece.Surfaces[SectionIndex]);
							++Drawn;
						}
					}
				}
				Triangles += Drawn;
			});
			return Triangles.load();
		}
	};

	/** A water body's spline, sampled in the world: WaterSplineSteps points a span, with the river's half-width at each. */
	void SampleWaterSpline(const FMantleWaterBodyData& Body, bool bClosed, TArray<FVector>& OutPoints, TArray<double>& OutHalfWidths)
	{
		const TArray<FMantleWaterSplineSample>& Points = Body.Points;
		const int32 Spans = bClosed ? Points.Num() : Points.Num() - 1;
		for (int32 Span = 0; Span < Spans; ++Span)
		{
			const FMantleWaterSplineSample& From = Points[Span];
			const FMantleWaterSplineSample& To = Points[(Span + 1) % Points.Num()];
			const bool bLinear = From.Type == TEXT("Linear");
			for (int32 Step = 0; Step < WaterSplineSteps; ++Step)
			{
				const double Alpha = static_cast<double>(Step) / WaterSplineSteps;
				const FVector Local = bLinear ? FMath::Lerp(From.Location, To.Location, Alpha)
					: FMath::CubicInterp(From.Location, From.LeaveTangent, To.Location, To.ArriveTangent, Alpha);
				OutPoints.Add(Body.SplineTransform.TransformPosition(Local));
				OutHalfWidths.Add(FMath::Lerp(static_cast<double>(From.Width), static_cast<double>(To.Width), Alpha));
			}
		}
		if (!bClosed && Points.Num() > 0)
		{
			OutPoints.Add(Body.SplineTransform.TransformPosition(Points.Last().Location));
			OutHalfWidths.Add(Points.Last().Width);
		}
	}

	/** Where a lake, river or custom body's water lies, for the area: its spline's points, a river's grown by its width. */
	FBox WaterBox(const FMantleWaterBodyData& Body)
	{
		FBox Box(ForceInit);
		if (Body.Points.Num() < 2)
		{
			return Box;
		}
		TArray<FVector> Points;
		TArray<double> HalfWidths;
		SampleWaterSpline(Body, Body.Type != EMantleWaterBodyKind::River, Points, HalfWidths);
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			const double Grow = Body.Type == EMantleWaterBodyKind::River ? HalfWidths[Index] : 0.0;
			Box += FBox::BuildAABB(Points[Index], FVector(Grow, Grow, 0.0));
		}
		return Box;
	}

	/** Draws Body's water: a lake's or a closed custom body's outline filled, a river's band, an ocean's extent; a custom body with a mesh, its mesh. */
	int64 DrawWater(const FMantleWaterBodyData& Body, const FMantleWaterBodies& Water, FMantleMinimapCanvas& Canvas, FMeshDraws& CustomMeshes,
		const FBox2D& Area)
	{
		int64 Triangles = 0;
		auto Triangle = [&Canvas, &Triangles](const FVector& A, const FVector& B, const FVector& C)
		{
			Canvas.DrawTriangle(A, B, C, EMantleMinimapSurface::Water);
			++Triangles;
		};
		if (Body.Type == EMantleWaterBodyKind::Ocean)
		{
			// The ocean's extent around the water zone (when it follows it) or the body, at the body's height.
			FVector Center = Body.Transform.GetLocation();
			if (Body.bOceanCentersOnZone && Water.Zones.Num() > 0)
			{
				Center = FVector(Water.Zones[0].Transform.GetLocation().X, Water.Zones[0].Transform.GetLocation().Y, Center.Z);
			}
			const FVector2D Half = Body.OceanExtents * 0.5;
			if (Half.X > 0.0 && Half.Y > 0.0)
			{
				const FVector A(Center.X - Half.X, Center.Y - Half.Y, Center.Z), B(Center.X + Half.X, Center.Y - Half.Y, Center.Z);
				const FVector C(Center.X + Half.X, Center.Y + Half.Y, Center.Z), D(Center.X - Half.X, Center.Y + Half.Y, Center.Z);
				Triangle(A, B, C);
				Triangle(A, C, D);
			}
			return Triangles;
		}
		if (Body.Type == EMantleWaterBodyKind::Custom && Body.Mesh.IsValid())
		{
			const int32 Mesh = CustomMeshes.MeshOf(Body.Mesh.Get());
			if (Mesh != INDEX_NONE)
			{
				FPiece Piece;
				Piece.Mesh = Mesh;
				Piece.Surfaces.Init(EMantleMinimapSurface::Water, CustomMeshes.Meshes[Mesh].Sections.Num());
				CustomMeshes.Place(CustomMeshes.Pieces.Add(MoveTemp(Piece)), Body.Transform, Area);
			}
			return Triangles;
		}
		if (Body.Points.Num() < 2)
		{
			return Triangles;
		}
		TArray<FVector> Points;
		TArray<double> HalfWidths;
		if (Body.Type == EMantleWaterBodyKind::River)
		{
			SampleWaterSpline(Body, Body.bClosedLoop, Points, HalfWidths);
			// A band HalfWidth either side of the line, square to it on the ground.
			TArray<FVector> Left, Right;
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				const FVector Ahead = Points[FMath::Min(Index + 1, Points.Num() - 1)] - Points[FMath::Max(Index - 1, 0)];
				FVector Side(-Ahead.Y, Ahead.X, 0.0);
				Side = Side.GetSafeNormal() * HalfWidths[Index];
				Left.Add(Points[Index] + Side);
				Right.Add(Points[Index] - Side);
			}
			for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
			{
				Triangle(Left[Index], Right[Index], Right[Index + 1]);
				Triangle(Left[Index], Right[Index + 1], Left[Index + 1]);
			}
			return Triangles;
		}
		// A lake, or a custom body without a mesh: its outline filled at its average height.
		SampleWaterSpline(Body, /*bClosed*/ true, Points, HalfWidths);
		TArray<FVector2D> Outline;
		double Height = 0.0;
		for (const FVector& Point : Points)
		{
			Outline.Add(FVector2D(Point));
			Height += Point.Z;
		}
		Height /= FMath::Max(1, Points.Num());
		const TArray<int32> Indices = FMantleMinimap::Triangulate(Outline);
		for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
		{
			Triangle(FVector(Outline[Indices[Index]], Height), FVector(Outline[Indices[Index + 1]], Height), FVector(Outline[Indices[Index + 2]], Height));
		}
		return Triangles;
	}

	/**
	 * Draws a landscape's ground: its quads, a few samples apart when the picture's pixels are larger than its spacing, each the
	 * surface its strongest paint layer's name says (ground otherwise). Painted holes are left open.
	 */
	int64 DrawLandscape(const FMantleTerrainSource& Source, FMantleMinimapCanvas& Canvas)
	{
		const int32 W = Source.Width, H = Source.Height;
		if (W < 2 || H < 2 || Source.Heights.Num() != W * H)
		{
			return 0;
		}
		const int32 Stride = FMath::Max(1, FMath::FloorToInt32(Canvas.GetGrid().CmPerPixel / FMath::Max(1.0, FMath::Min(Source.SpacingX, Source.SpacingY))));
		TArray<EMantleMinimapSurface> LayerSurfaces;
		for (const FName& Layer : Source.LayerNames)
		{
			const EMantleMinimapSurface Surface = FMantleMinimapCanvas::Classify(Layer.ToString());
			LayerSurfaces.Add(Surface == EMantleMinimapSurface::None || Surface == EMantleMinimapSurface::Building ? EMantleMinimapSurface::Ground : Surface);
		}
		const bool bHoles = Source.Holes.Num() == W * H;
		auto Corner = [&Source, W](int32 X, int32 Y)
		{
			const double Z = Source.Heights[Y * W + X] * Source.HeightScale + Source.HeightOffset;
			return Source.Transform.TransformPosition(FVector(X * Source.SpacingX, Y * Source.SpacingY, Z));
		};
		auto SurfaceAt = [&Source, &LayerSurfaces, W](int32 X, int32 Y)
		{
			int32 Strongest = INDEX_NONE;
			uint8 Most = 0;
			for (int32 Layer = 0; Layer < LayerSurfaces.Num() && Layer < Source.LayerWeights.Num(); ++Layer)
			{
				const TArray<uint8>& Weights = Source.LayerWeights[Layer];
				const uint8 Weight = Weights.IsValidIndex(Y * W + X) ? Weights[Y * W + X] : 0;
				if (Weight > Most)
				{
					Most = Weight;
					Strongest = Layer;
				}
			}
			return Strongest == INDEX_NONE ? EMantleMinimapSurface::Ground : LayerSurfaces[Strongest];
		};
		const int32 Rows = FMath::DivideAndRoundUp(H - 1, Stride);
		std::atomic<int64> Triangles(0);
		ParallelFor(Rows, [&](int32 Row)
		{
			const int32 Y = Row * Stride, Y2 = FMath::Min(Y + Stride, H - 1);
			int64 Drawn = 0;
			for (int32 X = 0; X < W - 1; X += Stride)
			{
				const int32 X2 = FMath::Min(X + Stride, W - 1);
				if (bHoles && (Source.Holes[Y * W + X] || Source.Holes[Y * W + X2] || Source.Holes[Y2 * W + X] || Source.Holes[Y2 * W + X2]))
				{
					continue;
				}
				const EMantleMinimapSurface Surface = SurfaceAt(X, Y);
				const FVector A = Corner(X, Y), B = Corner(X2, Y), C = Corner(X2, Y2), D = Corner(X, Y2);
				Canvas.DrawTriangle(A, B, C, Surface);
				Canvas.DrawTriangle(A, C, D, Surface);
				Drawn += 2;
			}
			Triangles += Drawn;
		});
		return Triangles.load();
	}

	FBox2D Flat(const FBox& Box)
	{
		return Box.IsValid != 0 ? FBox2D(FVector2D(Box.Min), FVector2D(Box.Max)) : FBox2D(ForceInit);
	}

	/** The creator's own picture, as Mantle Place Settings > Minimap chooses it: a custom thumbnail choice for the shared loader. */
	bool OwnPicture(UWorld* World, FMantleThumbnailChoice& OutChoice)
	{
		const AMantlePlaceSettingsActor* Settings = World ? AMantlePlaceSettingsActor::Find(World) : nullptr;
		if (!Settings || Settings->MapPicture != EMantlePlaceMapPicture::Own)
		{
			return false;
		}
		OutChoice = Settings->MapPictureChoice();
		return true;
	}

	void Warn(TArray<FMantleValidationIssue>& Issues, const FString& Problem, const FString& Remedy)
	{
		Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(), Problem, Remedy });
	}
}

TArray<FColor> FMantleMinimapPicture::Tile(int32 Row, int32 Column) const
{
	TArray<FColor> Out;
	if (!IsValid() || Row < 0 || Column < 0 || Row >= Grid.Rows || Column >= Grid.Columns)
	{
		return Out;
	}
	Out.SetNumUninitialized(Grid.TileWidth * Grid.TileHeight);
	for (int32 Y = 0; Y < Grid.TileHeight; ++Y)
	{
		const int64 From = static_cast<int64>(Row * Grid.TileHeight + Y) * Grid.Width + Column * Grid.TileWidth;
		FMemory::Memcpy(&Out[Y * Grid.TileWidth], &Pixels[From], Grid.TileWidth * sizeof(FColor));
	}
	return Out;
}

FBox FMantleMinimap::Area(UWorld* World)
{
	using namespace MantleMinimapPrivate;
	FBox Colliding(ForceInit), Everything(ForceInit);
	if (!World)
	{
		return Colliding;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsDrawnActor(Actor))
		{
			continue;
		}
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (const UStaticMeshComponent* Component : Components)
		{
			if (IsDrawnMesh(Component))
			{
				const FBox Box = Component->Bounds.GetBox();
				Everything += Box;
				if (Collides(Component))
				{
					Colliding += Box;
				}
			}
		}
	}
	// Spline meshes as Export bakes them (cached: the bake Check and Export use).
	const FMantleSplineBakeResult Bake = FMantleSplineMeshBake::Bake(World, FMantlePlaceAssetLimits::FromContract());
	for (const FMantleBakedSplineMesh& Baked : Bake.Meshes)
	{
		if (Baked.Mesh)
		{
			const FBox Box = Baked.Mesh->GetBoundingBox().TransformBy(Baked.Transform);
			Everything += Box;
			if (Baked.Collision == EMantleCollision::Blocking)
			{
				Colliding += Box;
			}
		}
	}
	// Every landscape proxy: a World Partition landscape's components are on its streaming proxies, not on the landscape actor.
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		const FBox Box = It->GetComponentsBoundingBox(/*bNonColliding*/ true);
		if (Box.IsValid != 0)
		{
			Colliding += Box;
		}
	}
	const FMantleWaterBodies Water = FMantleWaterBodyExport::Read(World);
	for (const FMantleWaterBodyData& Body : Water.Bodies)
	{
		if (Body.Type != EMantleWaterBodyKind::Ocean)
		{
			const FBox Box = WaterBox(Body);
			if (Box.IsValid != 0)
			{
				Colliding += Box;
			}
		}
	}
	return Colliding.IsValid != 0 ? Colliding : Everything;
}

bool FMantleMinimap::Draw(UWorld* World, FMantleMinimapPicture& Out, FString& OutError)
{
	using namespace MantleMinimapPrivate;
	Out = FMantleMinimapPicture();
	const double Started = FPlatformTime::Seconds();
	const FBox Area3D = Area(World);
	Out.Grid = FMantleMinimapGrid::Plan(Flat(Area3D));
	if (!Out.Grid.IsValid())
	{
		OutError = TEXT("the level has nothing a map could show");
		return false;
	}
	const FBox2D Picture(FVector2D(Out.Grid.MinX, Out.Grid.MinY), FVector2D(Out.Grid.MaxX, Out.Grid.MaxY));
	const double AreaSeconds = FPlatformTime::Seconds() - Started;

	// Meshes: every placed static mesh (each instance of an instanced one) and every baked spline mesh, read once per mesh.
	double Clock = FPlatformTime::Seconds();
	FMeshDraws Meshes;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsDrawnActor(Actor))
		{
			continue;
		}
		const bool bFoliage = Actor->GetClass()->GetName().Contains(TEXT("Foliage"));
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (const UStaticMeshComponent* Component : Components)
		{
			if (!IsDrawnMesh(Component))
			{
				continue;
			}
			FPiece Piece;
			Piece.Mesh = Meshes.MeshOf(Component->GetStaticMesh());
			if (Piece.Mesh == INDEX_NONE)
			{
				continue;
			}
			// A name that says nothing leaves the surface Unknown: ground, or a building where it stands tall (FMantleMinimapCanvas::Shade).
			for (const FIntVector3& Section : Meshes.Meshes[Piece.Mesh].Sections)
			{
				const UMaterialInterface* Material = Component->GetMaterial(Section.Z);
				Piece.Surfaces.Add(SurfaceOfNames({ Material ? Material->GetName() : FString(), Component->GetStaticMesh()->GetName(), Actor->GetActorLabel() },
					bFoliage ? EMantleMinimapSurface::Green : EMantleMinimapSurface::Unknown));
			}
			const int32 PieceIndex = Meshes.Pieces.Add(MoveTemp(Piece));
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
			{
				for (int32 Instance = 0; Instance < Instanced->GetInstanceCount(); ++Instance)
				{
					FTransform Transform;
					if (Instanced->GetInstanceTransform(Instance, Transform, /*bWorldSpace*/ true))
					{
						Meshes.Place(PieceIndex, Transform, Picture);
					}
				}
			}
			else
			{
				Meshes.Place(PieceIndex, Component->GetComponentTransform(), Picture);
			}
		}
	}
	const FMantleSplineBakeResult Bake = FMantleSplineMeshBake::Bake(World, FMantlePlaceAssetLimits::FromContract());
	int32 SplineMeshes = 0;
	for (const FMantleBakedSplineMesh& Baked : Bake.Meshes)
	{
		if (!Baked.Mesh || !Baked.bVisible)
		{
			continue;
		}
		FPiece Piece;
		Piece.Mesh = Meshes.MeshOf(Baked.Mesh);
		if (Piece.Mesh == INDEX_NONE)
		{
			continue;
		}
		const TArray<FStaticMaterial>& Materials = Baked.Mesh->GetStaticMaterials();
		for (const FIntVector3& Section : Meshes.Meshes[Piece.Mesh].Sections)
		{
			const UMaterialInterface* Material = Materials.IsValidIndex(Section.Z) ? Materials[Section.Z].MaterialInterface.Get() : nullptr;
			Piece.Surfaces.Add(SurfaceOfNames({ Material ? Material->GetName() : FString(), Baked.OwnerLabel }, EMantleMinimapSurface::Unknown));
		}
		Meshes.Place(Meshes.Pieces.Add(MoveTemp(Piece)), Baked.Transform, Picture);
		++SplineMeshes;
	}

	// Water Bodies: lakes and custom bodies filled, rivers as bands, an ocean's extent; a custom body's own mesh joins the meshes.
	FMantleMinimapCanvas Canvas(Out.Grid);
	const FMantleWaterBodies Water = FMantleWaterBodyExport::Read(World);
	int64 WaterTriangles = 0;
	for (const FMantleWaterBodyData& Body : Water.Bodies)
	{
		WaterTriangles += DrawWater(Body, Water, Canvas, Meshes, Picture);
	}
	const double ReadSeconds = FPlatformTime::Seconds() - Clock;

	Clock = FPlatformTime::Seconds();
	const int64 MeshTriangles = Meshes.DrawAll(Canvas);
	int32 Landscapes = 0;
	int64 LandscapeTriangles = 0;
	for (const AActor* Landscape : FMantleTerrainExport::CollectLandscapes(World))
	{
		FMantleTerrainSource Source;
		FString Problem;
		if (FMantleTerrainExport::ReadLandscape(Landscape, Source, Problem))
		{
			LandscapeTriangles += DrawLandscape(Source, Canvas);
			++Landscapes;
		}
		else
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_MINIMAP landscape %s left out: %s"), *Landscape->GetActorLabel(), *Problem);
		}
	}
	const double DrawSeconds = FPlatformTime::Seconds() - Clock;

	Clock = FPlatformTime::Seconds();
	Out.Pixels = Canvas.Shade();
	const double ShadeSeconds = FPlatformTime::Seconds() - Clock;
	Out.Source = FString::Printf(TEXT("drawn map %dx%d at %.2f m per pixel (%dx%d tiles) in %.2f s (area %.2f, read %.2f, draw %.2f, shade %.2f): ")
		TEXT("%d meshes, %d placements, %d baked spline meshes, %d landscapes, %d water bodies, %lld triangles, %d unreadable meshes"),
		Out.Grid.Width, Out.Grid.Height, Out.Grid.CmPerPixel / 100.0, Out.Grid.Columns, Out.Grid.Rows, FPlatformTime::Seconds() - Started, AreaSeconds,
		ReadSeconds, DrawSeconds, ShadeSeconds, Meshes.Meshes.Num(), Meshes.Draws.Num(), SplineMeshes, Landscapes, Water.Bodies.Num(),
		MeshTriangles + LandscapeTriangles + WaterTriangles, Meshes.Unreadable);
	return Out.IsValid();
}

FString FMantleMinimap::DescribeShape(int32 Width, int32 Height, const FMantleMinimapGrid& Grid)
{
	if (Width <= 0 || Height <= 0 || !Grid.IsValid())
	{
		return FString();
	}
	const double Picture = static_cast<double>(Width) / Height;
	const double Map = static_cast<double>(Grid.Width) / Grid.Height;
	if (FMath::Abs(Picture / Map - 1.0) <= ShapeTolerance)
	{
		return FString();
	}
	return FString::Printf(TEXT("Your map picture is %d x %d pixels, but the place's map is %.0f x %.0f m (%s): the picture is stretched to fit, so it looks squashed."),
		Width, Height, Grid.Width * Grid.CmPerPixel / 100.0, Grid.Height * Grid.CmPerPixel / 100.0,
		Map > Picture ? TEXT("wider than the picture") : TEXT("taller than the picture"));
}

bool FMantleMinimap::FromImage(TConstArrayView<FColor> Image, int32 Width, int32 Height, const FMantleMinimapGrid& Grid, TArray<FColor>& OutPixels,
	FString& OutWarning, FString& OutError)
{
	OutPixels.Reset();
	OutWarning.Reset();
	if (Width <= 0 || Height <= 0 || Image.Num() != Width * Height)
	{
		OutError = TEXT("the picture has no pixels");
		return false;
	}
	if (!Grid.IsValid())
	{
		OutError = TEXT("the level has nothing a map could cover");
		return false;
	}
	// Smooth stretching: each output pixel's center, found in the picture, blended from its four nearest pixels.
	OutPixels.SetNumUninitialized(Grid.Width * Grid.Height);
	const double ScaleX = static_cast<double>(Width) / Grid.Width, ScaleY = static_cast<double>(Height) / Grid.Height;
	for (int32 Y = 0; Y < Grid.Height; ++Y)
	{
		const double SY = FMath::Clamp((Y + 0.5) * ScaleY - 0.5, 0.0, Height - 1.0);
		const int32 Y0 = FMath::FloorToInt32(SY), Y1 = FMath::Min(Y0 + 1, Height - 1);
		const float FY = static_cast<float>(SY - Y0);
		for (int32 X = 0; X < Grid.Width; ++X)
		{
			const double SX = FMath::Clamp((X + 0.5) * ScaleX - 0.5, 0.0, Width - 1.0);
			const int32 X0 = FMath::FloorToInt32(SX), X1 = FMath::Min(X0 + 1, Width - 1);
			const float FX = static_cast<float>(SX - X0);
			auto At = [&Image, Width](int32 PX, int32 PY) { return Image[PY * Width + PX].ReinterpretAsLinear(); };
			const FLinearColor Top = FMath::Lerp(At(X0, Y0), At(X1, Y0), FX), Bottom = FMath::Lerp(At(X0, Y1), At(X1, Y1), FX);
			FColor Pixel = FMath::Lerp(Top, Bottom, FY).QuantizeRound();
			Pixel.A = 255;
			OutPixels[Y * Grid.Width + X] = Pixel;
		}
	}
	OutWarning = DescribeShape(Width, Height, Grid);
	return true;
}

bool FMantleMinimap::Make(UWorld* World, FMantleMinimapPicture& Out, FString& OutError)
{
	using namespace MantleMinimapPrivate;
	FMantleThumbnailChoice Own;
	if (!OwnPicture(World, Own))
	{
		return Draw(World, Out, OutError);
	}
	Out = FMantleMinimapPicture();
	TArray<FColor> Image;
	int32 Width = 0, Height = 0;
	FString Why;
	if (!Own.HasCustomImage() || !FMantleThumbnailResolver::LoadCustom(Own, Image, Width, Height, Why))
	{
		OutError = FString::Printf(TEXT("your own map picture could not be read (%s)"), Own.HasCustomImage() ? *Why : TEXT("none is chosen"));
		return false;
	}
	Out.Grid = FMantleMinimapGrid::Plan(Flat(Area(World)));
	if (!FromImage(Image, Width, Height, Out.Grid, Out.Pixels, Out.Warning, OutError))
	{
		return false;
	}
	Out.Source = FString::Printf(TEXT("own map picture %dx%d stretched to %dx%d (%dx%d tiles)"), Width, Height, Out.Grid.Width, Out.Grid.Height,
		Out.Grid.Columns, Out.Grid.Rows);
	return true;
}

bool FMantleMinimap::Convert(UWorld* World, FMantlePlaceAssetConverter& Converter, FMantlePlaceAssetSet& Set)
{
	using namespace MantleMinimapPrivate;
	const FString Remedy = TEXT("Publish again; if it keeps happening, choose your own picture on Mantle Place Settings > Minimap.");
	FMantleMinimapPicture Picture;
	FString Error;
	if (!Make(World, Picture, Error))
	{
		Warn(Set.Issues, FString::Printf(TEXT("The place's map picture could not be made (%s), so the minimap shows no map."), *Error), Remedy);
		return false;
	}
	if (!Picture.Warning.IsEmpty())
	{
		Warn(Set.Issues, Picture.Warning, TEXT("Use a picture with the map's shape (Mantle Place Settings > Minimap > Show the Map Picture prints it)."));
	}
	TArray<TArray<FString>> Tiles;
	for (int32 Row = 0; Row < Picture.Grid.Rows; ++Row)
	{
		TArray<FString>& Keys = Tiles.AddDefaulted_GetRef();
		for (int32 Column = 0; Column < Picture.Grid.Columns; ++Column)
		{
			FString Key;
			if (!Converter.ConvertMinimapTile(Picture.Tile(Row, Column), Picture.Grid.TileWidth, Picture.Grid.TileHeight, Set, Key))
			{
				Warn(Set.Issues, TEXT("The place's map picture could not be prepared for upload, so the minimap shows no map."), Remedy);
				return false;
			}
			Keys.Add(Key);
		}
	}
	Set.MinimapTiles = MoveTemp(Tiles);
	Set.MinimapBounds = FBox2D(FVector2D(Picture.Grid.MinX, Picture.Grid.MinY), FVector2D(Picture.Grid.MaxX, Picture.Grid.MaxY));
	UE_LOG(LogMantle, Display, TEXT("MANTLE_MINIMAP %s; bounds x=%.1f..%.1f y=%.1f..%.1f"), *Picture.Source, Picture.Grid.MinX, Picture.Grid.MaxX,
		Picture.Grid.MinY, Picture.Grid.MaxY);
	return true;
}

void FMantleMinimap::Check(UWorld* World, TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantleMinimapPrivate;
	FMantleThumbnailChoice Own;
	if (!OwnPicture(World, Own))
	{
		return;
	}
	const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World);
	const FString Label = Settings ? Settings->GetActorLabel() : FString();
	if (!Own.HasCustomImage())
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label,
			TEXT("Map Picture is set to My own picture, but no picture is chosen."),
			TEXT("Choose a texture or a PNG file under Mantle Place Settings > Minimap, or set Map Picture back to Made automatically.") });
		return;
	}
	TArray<FColor> Image;
	int32 Width = 0, Height = 0;
	FString Why;
	if (!FMantleThumbnailResolver::LoadCustom(Own, Image, Width, Height, Why))
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label,
			FString::Printf(TEXT("Your own map picture can't be read: %s."), *Why),
			TEXT("Choose a texture or a PNG file that exists under Mantle Place Settings > Minimap, or set Map Picture back to Made automatically.") });
		return;
	}
	const FString Shape = DescribeShape(Width, Height, FMantleMinimapGrid::Plan(Flat(Area(World))));
	if (!Shape.IsEmpty())
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, Label, Shape,
			TEXT("It still publishes. Use a picture with the map's shape: Mantle Place Settings > Minimap > Show the Map Picture prints the map's size.") });
	}
}

TArray<int32> FMantleMinimap::Triangulate(TConstArrayView<FVector2D> Corners)
{
	TArray<int32> Out;
	const int32 Count = Corners.Num();
	if (Count < 3)
	{
		return Out;
	}
	// Ear clipping. The winding decides which corners are convex.
	double Twice = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D& A = Corners[Index];
		const FVector2D& B = Corners[(Index + 1) % Count];
		Twice += A.X * B.Y - B.X * A.Y;
	}
	const double Winding = Twice >= 0.0 ? 1.0 : -1.0;
	auto Cross = [&Corners](int32 A, int32 B, int32 C)
	{
		return (Corners[B].X - Corners[A].X) * (Corners[C].Y - Corners[A].Y) - (Corners[B].Y - Corners[A].Y) * (Corners[C].X - Corners[A].X);
	};
	TArray<int32> Left;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Left.Add(Index);
	}
	int32 Guard = Count * Count;
	while (Left.Num() > 3 && Guard-- > 0)
	{
		bool bCut = false;
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			const int32 A = Left[(Index + Left.Num() - 1) % Left.Num()], B = Left[Index], C = Left[(Index + 1) % Left.Num()];
			if (Cross(A, B, C) * Winding <= 0.0)
			{
				continue;
			}
			// An ear holds no other corner.
			bool bEmpty = true;
			for (const int32 Other : Left)
			{
				if (Other == A || Other == B || Other == C)
				{
					continue;
				}
				if (Cross(A, B, Other) * Winding >= 0.0 && Cross(B, C, Other) * Winding >= 0.0 && Cross(C, A, Other) * Winding >= 0.0)
				{
					bEmpty = false;
					break;
				}
			}
			if (bEmpty)
			{
				Out.Append({ A, B, C });
				Left.RemoveAt(Index);
				bCut = true;
				break;
			}
		}
		if (!bCut)
		{
			break;
		}
	}
	if (Left.Num() == 3)
	{
		Out.Append({ Left[0], Left[1], Left[2] });
	}
	return Out;
}
