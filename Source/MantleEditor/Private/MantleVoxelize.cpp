// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelize.h"

#include "MantleCosmeticExport.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"
#include "MantleSkyMesh.h"
#include "MantleVoxelGrid.h"
#include "MantleVoxelTypes.h"
#include "MantleVoxelWorldActor.h"
#include "MantleVoxelWorldFile.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IDesktopPlatform.h"
#include "ImageCore.h"
#include "LevelEditorViewport.h"
#include "MeshDescription.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "MantleVoxelize"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVoxelizePrivate
{
	/**
	 * A mesh's triangles in its own space (LOD 0 of its source data), with corners welded by position, so a cube whose faces
	 * were imported unwelded still counts as closed. Closed: every edge has exactly two triangles, so a ray's crossings tell
	 * inside from outside (#64).
	 */
	struct FMeshShape
	{
		TArray<FVector> Positions;
		TArray<FIntVector> Triangles;
		bool bClosed = false;
	};

	/** One mesh component or instance: its world bounds, its shape and where it sits, its block type and its source actor. */
	struct FFill
	{
		FBox Box;
		int32 Type = 0;
		int32 Source = 0;
		/** Null when the mesh has no source data to read: the whole box fills, as a cube does. */
		const FMeshShape* Shape = nullptr;
		FTransform Transform;
	};

	/** Mesh's shape, read once per mesh; null when it has no source triangles (a cooked-only mesh). */
	const FMeshShape* ShapeOf(UStaticMesh* Mesh, TMap<UStaticMesh*, TUniquePtr<FMeshShape>>& Shapes)
	{
		if (const TUniquePtr<FMeshShape>* Found = Shapes.Find(Mesh))
		{
			return Found->Get();
		}
		TUniquePtr<FMeshShape>& Shape = Shapes.Add(Mesh);
		const FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!Description || Description->Triangles().Num() == 0)
		{
			return nullptr;
		}
		Shape = MakeUnique<FMeshShape>();
		const TVertexAttributesRef<const FVector3f> Positions = Description->GetVertexPositions();
		// Welded to a hundredth of a millimetre.
		TMap<FIntVector, int32> Welded;
		TMap<FVertexID, int32> IndexOf;
		for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
		{
			const FVector Position(Positions[Vertex]);
			const FIntVector Key(FMath::RoundToInt(Position.X * 1000.0), FMath::RoundToInt(Position.Y * 1000.0), FMath::RoundToInt(Position.Z * 1000.0));
			int32* Index = Welded.Find(Key);
			if (!Index)
			{
				Index = &Welded.Add(Key, Shape->Positions.Add(Position));
			}
			IndexOf.Add(Vertex, *Index);
		}
		TMap<TPair<int32, int32>, int32> EdgeUses;
		for (const FTriangleID Triangle : Description->Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> Corners = Description->GetTriangleVertices(Triangle);
			const FIntVector Tri(IndexOf[Corners[0]], IndexOf[Corners[1]], IndexOf[Corners[2]]);
			if (Tri.X == Tri.Y || Tri.Y == Tri.Z || Tri.Z == Tri.X)
			{
				continue; // collapsed by the weld: no area, crosses nothing
			}
			Shape->Triangles.Add(Tri);
			for (int32 Side = 0; Side < 3; ++Side)
			{
				const int32 A = Tri[Side];
				const int32 B = Tri[(Side + 1) % 3];
				++EdgeUses.FindOrAdd(TPair<int32, int32>(FMath::Min(A, B), FMath::Max(A, B)));
			}
		}
		Shape->bClosed = Shape->Triangles.Num() >= 4;
		for (const TPair<TPair<int32, int32>, int32>& Edge : EdgeUses)
		{
			if (Edge.Value != 2)
			{
				Shape->bClosed = false;
				break;
			}
		}
		return Shape.Get();
	}

	/** A block type as Voxelize finds it: one (mesh, first material) pair. */
	struct FSourceType
	{
		UStaticMesh* Mesh = nullptr;
		UMaterialInterface* Material = nullptr;
	};

	/** A little under a millimetre, in cells: a cube's face exactly on a cell centre still counts as covering it. */
	constexpr double CellSlack = 0.001;

	/** The cells (Cell cm wide) along one axis whose centres lie in [Min, Max] cm: [OutLo, OutHi], empty when OutLo > OutHi. */
	void CellRange(double Min, double Max, int64& OutLo, int64& OutHi, double Cell = MantleVoxel::BlockSize)
	{
		// Held well inside int64, so a mesh with absurd bounds is refused as too big rather than overflowing.
		constexpr double Limit = 1.0e12;
		OutLo = static_cast<int64>(FMath::Clamp(FMath::CeilToDouble(Min / Cell - 0.5 - CellSlack), -Limit, Limit));
		OutHi = static_cast<int64>(FMath::Clamp(FMath::FloorToDouble(Max / Cell - 0.5 + CellSlack), -Limit, Limit));
	}

	/** Every static mesh component of Actor (each instance of an instanced one) as a fill, adding its (mesh, material) type. */
	void CollectFills(AActor* Actor, int32 Source, bool bSmooth, TArray<FFill>& Fills, TArray<FSourceType>& Types, TMap<TPair<UStaticMesh*, UMaterialInterface*>, int32>& TypeOf,
		TMap<UStaticMesh*, TUniquePtr<FMeshShape>>& Shapes)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components;
		Actor->GetComponents(Components);
		for (UStaticMeshComponent* Component : Components)
		{
			UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			// A sky dome is left out as the exporter leaves it out of a place (FMantleSkyMesh): filled, it is a solid mass of blocks (#64).
			if (!Mesh || Component->IsEditorOnly() || FMantleSkyMesh::IsSkyMesh(Component))
			{
				continue;
			}
			UMaterialInterface* Material = Component->GetMaterial(0);
			// Smooth ground has no cube faces to tell meshes apart by: one ground material per material.
			const TPair<UStaticMesh*, UMaterialInterface*> Key(bSmooth ? nullptr : Mesh, Material);
			int32 Type = INDEX_NONE;
			if (const int32* Found = TypeOf.Find(Key))
			{
				Type = *Found;
			}
			else
			{
				Type = Types.Add(FSourceType{ Mesh, Material });
				TypeOf.Add(Key, Type);
			}
			const FBox MeshBox = Mesh->GetBoundingBox();
			const FMeshShape* Shape = ShapeOf(Mesh, Shapes);
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
			{
				for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
				{
					FTransform Instance;
					if (Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ true))
					{
						Fills.Add(FFill{ MeshBox.TransformBy(Instance), Type, Source, Shape, Instance });
					}
				}
				continue;
			}
			Fills.Add(FFill{ MeshBox.TransformBy(Component->GetComponentTransform()), Type, Source, Shape, Component->GetComponentTransform() });
		}
	}

	/**
	 * Calls Set(X, Y, Z) for each cell (world cell coordinates, within [Lo, Hi]) that Fill makes solid (#64):
	 * - no shape: every cell of the box, as before;
	 * - a closed mesh: each cell whose centre is inside it, from one ray along X per row of cells, filled between
	 *   alternate crossings;
	 * - an open mesh, or a closed one too thin to hold a cell centre (a fence board): the cells its surface passes through.
	 */
	template <typename FSetFn>
	void FillCells(const FFill& Fill, const int64 Lo[3], const int64 Hi[3], const double Block, FSetFn&& Set)
	{
		if (!Fill.Shape)
		{
			for (int64 Z = Lo[2]; Z <= Hi[2]; ++Z)
			{
				for (int64 Y = Lo[1]; Y <= Hi[1]; ++Y)
				{
					for (int64 X = Lo[0]; X <= Hi[0]; ++X)
					{
						Set(X, Y, Z);
					}
				}
			}
			return;
		}
		TArray<FVector> World;
		World.Reserve(Fill.Shape->Positions.Num());
		for (const FVector& Position : Fill.Shape->Positions)
		{
			World.Add(Fill.Transform.TransformPosition(Position));
		}
		bool bAny = false;
		if (Fill.Shape->bClosed)
		{
			// Rows are offset a hair off the cell centre, so a ray never runs exactly along an edge or through a corner.
			constexpr double OffsetY = 0.0137;
			constexpr double OffsetZ = 0.0071;
			const int64 RowsY = Hi[1] - Lo[1] + 1;
			const int64 RowsZ = Hi[2] - Lo[2] + 1;
			TArray<TArray<double>> Crossings;
			Crossings.SetNum(RowsY * RowsZ);
			for (const FIntVector& Tri : Fill.Shape->Triangles)
			{
				const FVector& A = World[Tri.X];
				const FVector& B = World[Tri.Y];
				const FVector& C = World[Tri.Z];
				int64 YLo, YHi, ZLo, ZHi;
				CellRange(FMath::Min3(A.Y, B.Y, C.Y) - OffsetY, FMath::Max3(A.Y, B.Y, C.Y) - OffsetY, YLo, YHi, Block);
				CellRange(FMath::Min3(A.Z, B.Z, C.Z) - OffsetZ, FMath::Max3(A.Z, B.Z, C.Z) - OffsetZ, ZLo, ZHi, Block);
				YLo = FMath::Max(YLo, Lo[1]); YHi = FMath::Min(YHi, Hi[1]);
				ZLo = FMath::Max(ZLo, Lo[2]); ZHi = FMath::Min(ZHi, Hi[2]);
				// The triangle's projection on the YZ plane; one edge-on to the ray's direction crosses nothing.
				const double Det = (B.Y - A.Y) * (C.Z - A.Z) - (C.Y - A.Y) * (B.Z - A.Z);
				if (FMath::Abs(Det) < UE_DOUBLE_SMALL_NUMBER)
				{
					continue;
				}
				for (int64 Z = ZLo; Z <= ZHi; ++Z)
				{
					const double Pz = (static_cast<double>(Z) + 0.5) * Block + OffsetZ;
					for (int64 Y = YLo; Y <= YHi; ++Y)
					{
						const double Py = (static_cast<double>(Y) + 0.5) * Block + OffsetY;
						const double U = ((Py - A.Y) * (C.Z - A.Z) - (C.Y - A.Y) * (Pz - A.Z)) / Det;
						const double V = ((B.Y - A.Y) * (Pz - A.Z) - (Py - A.Y) * (B.Z - A.Z)) / Det;
						if (U < 0.0 || V < 0.0 || U + V > 1.0)
						{
							continue;
						}
						Crossings[(Z - Lo[2]) * RowsY + (Y - Lo[1])].Add(A.X + U * (B.X - A.X) + V * (C.X - A.X));
					}
				}
			}
			for (int64 Row = 0; Row < Crossings.Num(); ++Row)
			{
				TArray<double>& Xs = Crossings[Row];
				Xs.Sort();
				const int64 Y = Lo[1] + Row % RowsY;
				const int64 Z = Lo[2] + Row / RowsY;
				// In and out in pairs; an odd one left over (a crack the weld missed) is dropped.
				for (int32 Index = 0; Index + 1 < Xs.Num(); Index += 2)
				{
					int64 XLo, XHi;
					CellRange(Xs[Index], Xs[Index + 1], XLo, XHi, Block);
					for (int64 X = FMath::Max(XLo, Lo[0]); X <= FMath::Min(XHi, Hi[0]); ++X)
					{
						Set(X, Y, Z);
						bAny = true;
					}
				}
			}
		}
		if (bAny)
		{
			return;
		}
		// The surface: points across each triangle no further apart than half a block, each making its cell solid.
		const double Spacing = Block * 0.5;
		for (const FIntVector& Tri : Fill.Shape->Triangles)
		{
			const FVector& A = World[Tri.X];
			const FVector& B = World[Tri.Y];
			const FVector& C = World[Tri.Z];
			const FVector Centroid = (A + B + C) / 3.0;
			const double Longest = FMath::Max3(FVector::Dist(A, B), FVector::Dist(B, C), FVector::Dist(C, A));
			const int32 Steps = FMath::Clamp(FMath::CeilToInt(Longest / Spacing), 1, 4096);
			for (int32 I = 0; I <= Steps; ++I)
			{
				for (int32 J = 0; I + J <= Steps; ++J)
				{
					const double U = static_cast<double>(I) / Steps;
					const double V = static_cast<double>(J) / Steps;
					// A hair towards the middle, so a mesh's outer edge on a cell boundary doesn't spill into the next cell.
				const FVector OnTriangle = A + U * (B - A) + V * (C - A);
				const FVector P = OnTriangle + (Centroid - OnTriangle).GetSafeNormal() * 0.01;
					const int64 X = static_cast<int64>(FMath::FloorToDouble(P.X / Block));
					const int64 Y = static_cast<int64>(FMath::FloorToDouble(P.Y / Block));
					const int64 Z = static_cast<int64>(FMath::FloorToDouble(P.Z / Block));
					if (X >= Lo[0] && X <= Hi[0] && Y >= Lo[1] && Y <= Hi[1] && Z >= Lo[2] && Z <= Hi[2])
					{
						Set(X, Y, Z);
					}
				}
			}
		}
	}

	/** How a material's blend mode draws as a block. */
	EMantleVoxelBlockRender RenderFor(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return EMantleVoxelBlockRender::Opaque;
		}
		switch (Material->GetBlendMode())
		{
		case BLEND_Masked: return EMantleVoxelBlockRender::Cutout;
		case BLEND_Translucent:
		case BLEND_Additive:
		case BLEND_Modulate:
		case BLEND_AlphaComposite:
		case BLEND_AlphaHoldout:
			return EMantleVoxelBlockRender::Translucent;
		default:
			return EMantleVoxelBlockRender::Opaque;
		}
	}

	/**
	 * A Side x Side tile texture of Pixels: in the transient package when Folder is empty, else Folder/AssetName in the project's
	 * content, replacing the one a previous Voxelize saved there, and saved. Null, with OutError, when it cannot be saved.
	 */
	UTexture2D* MakeTile(const FString& Folder, const FString& AssetName, int32 Side, const TArray<FColor>& Pixels, FString& OutError)
	{
		UTexture2D* Texture = nullptr;
		UPackage* Package = nullptr;
		bool bNew = false;
		if (Folder.IsEmpty())
		{
			Package = GetTransientPackage();
			Texture = NewObject<UTexture2D>(Package, MakeUniqueObjectName(Package, UTexture2D::StaticClass(), FName(*AssetName)), RF_Transient);
			bNew = true;
		}
		else
		{
			const FString PackageName = Folder / AssetName;
			if (!FindPackage(nullptr, *PackageName) && FPackageName::DoesPackageExist(PackageName))
			{
				LoadPackage(nullptr, *PackageName, LOAD_None);
			}
			Package = CreatePackage(*PackageName);
			Texture = FindObject<UTexture2D>(Package, *AssetName);
			if (!Texture)
			{
				Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
				bNew = true;
			}
		}
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("the tile texture %s could not be made"), *AssetName);
			return nullptr;
		}

		bool bOpaque = true;
		for (const FColor& Pixel : Pixels)
		{
			bOpaque &= Pixel.A == 255;
		}
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Side, Side, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = true;
		// Pixel art: sharp texels, no streaming of a texture this small.
		Texture->LODGroup = TEXTUREGROUP_Pixels2D;
		Texture->Filter = TF_Nearest;
		Texture->CompressionNoAlpha = bOpaque;
		Texture->PostEditChange();

		if (Package == GetTransientPackage())
		{
			return Texture;
		}
		if (bNew)
		{
			FAssetRegistryModule::AssetCreated(Texture);
		}
		Package->MarkPackageDirty();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Texture, *Filename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("the tile texture %s could not be saved to %s (is the file read-only?)"), *AssetName, *Filename);
			return nullptr;
		}
		return Texture;
	}

	/** Material's tile: its base colour texture rendered down to Side x Side, or a flat tile of its base colour. */
	TArray<FColor> TilePixels(UMaterialInterface* Material, int32 Side)
	{
		TArray<FColor> Pixels;
		if (UTexture2D* Texture = FMantleVoxelize::FindBaseColorTexture(Material); Texture && Texture->Source.IsValid())
		{
			FImage Image;
			if (Texture->Source.GetMipImage(Image, 0))
			{
				FImage Linear;
				Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
				Pixels = FMantleVoxelize::ResampleSquare(Linear.AsRGBA32F(), Linear.SizeX, Linear.SizeY, Side);
			}
		}
		if (Pixels.Num() != Side * Side)
		{
			Pixels.Init(FMantleVoxelize::FindBaseColor(Material).ToFColor(/*bSRGB*/ true), Side * Side);
		}
		return Pixels;
	}

	/** Spawns the world actor at Origin (rotation 0, scale 1), labelled. */
	AMantleVoxelWorldActor* SpawnWorld(UWorld* World, const FVector& Origin)
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AMantleVoxelWorldActor* Actor = World->SpawnActor<AMantleVoxelWorldActor>(Origin, FRotator::ZeroRotator, Parameters);
		if (Actor)
		{
			Actor->SetActorLabel(TEXT("Voxel World"));
		}
		return Actor;
	}

	/** One notification in the editor's corner: what was made, or why not. */
	void Notify(const FText& Title, const FString& Detail, bool bSuccess)
	{
		FNotificationInfo Info(Title);
		Info.SubText = FText::FromString(Detail);
		Info.ExpireDuration = bSuccess ? 8.0f : 12.0f;
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
		UE_LOG(LogMantle, Log, TEXT("%s %s"), *Title.ToString(), *Detail);
	}

	/** "grass, dirt, stone and 4 more". */
	FString DescribeTypes(const TArray<FString>& Names)
	{
		constexpr int32 Shown = 6;
		TArray<FString> Listed(Names.GetData(), FMath::Min(Names.Num(), Shown));
		FString Text = FString::Join(Listed, TEXT(", "));
		if (Names.Num() > Shown)
		{
			Text += FString::Printf(TEXT(" and %d more"), Names.Num() - Shown);
		}
		return Text;
	}

	/** Where an imported world goes: in front of the viewport's camera, on the 100 cm grid. */
	FVector ImportLocation()
	{
		FVector Location = FVector::ZeroVector;
		if (GCurrentLevelEditingViewportClient)
		{
			const FRotator Facing(0.0, GCurrentLevelEditingViewportClient->GetViewRotation().Yaw, 0.0);
			Location = GCurrentLevelEditingViewportClient->GetViewLocation() + Facing.Vector() * 1000.0;
		}
		return FVector(FMath::GridSnap(Location.X, MantleVoxel::BlockSize), FMath::GridSnap(Location.Y, MantleVoxel::BlockSize),
			FMath::GridSnap(Location.Z, MantleVoxel::BlockSize));
	}
}

FString FMantleVoxelize::MakeTypeName(const FString& MeshName, TSet<FString>& Used)
{
	FString Lower = MeshName.ToLower();
	// The Unreal naming prefix (a mesh's, or a smooth ground material's) says nothing about the block.
	for (const TCHAR* Prefix : { TEXT("sm_"), TEXT("mi_"), TEXT("m_") })
	{
		const int32 Length = FCString::Strlen(Prefix);
		if (Lower.StartsWith(Prefix) && Lower.Len() > Length)
		{
			Lower.RightChopInline(Length);
			break;
		}
	}
	FString Name;
	for (const TCHAR C : Lower)
	{
		if ((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))
		{
			Name.AppendChar(C);
		}
		else if (Name.Len() > 0 && Name[Name.Len() - 1] != TEXT('_'))
		{
			Name.AppendChar(TEXT('_'));
		}
	}
	const int32 MaxLength = FMantlePlaceLimits::MaxVoxelBlockNameLength;
	Name = Name.Left(MaxLength);
	while (Name.EndsWith(TEXT("_")))
	{
		Name.LeftChopInline(1);
	}
	if (Name.IsEmpty())
	{
		Name = TEXT("block");
	}
	else if (Name == TEXT("air"))
	{
		Name = TEXT("air_block");
	}
	FString Candidate = Name;
	for (int32 Suffix = 2; Used.Contains(Candidate); ++Suffix)
	{
		const FString Tail = FString::Printf(TEXT("_%d"), Suffix);
		Candidate = Name.Left(MaxLength - Tail.Len()) + Tail;
	}
	Used.Add(Candidate);
	return Candidate;
}

FString FMantleVoxelize::TileFolderFor(const UWorld* World)
{
	FString Level = World ? World->GetName() : FString(TEXT("Level"));
	for (TCHAR& C : Level)
	{
		C = FChar::IsAlnum(C) || C == TEXT('_') ? C : TEXT('_');
	}
	return FString::Printf(TEXT("/Game/MantleVoxelize/%s"), *Level);
}

TArray<FColor> FMantleVoxelize::ResampleSquare(TConstArrayView64<FLinearColor> Pixels, int32 Width, int32 Height, int32 Side)
{
	TArray<FColor> Out;
	if (Width <= 0 || Height <= 0 || Side <= 0 || Pixels.Num() < static_cast<int64>(Width) * Height)
	{
		return Out;
	}
	Out.SetNumUninitialized(Side * Side);
	for (int32 Y = 0; Y < Side; ++Y)
	{
		// The source rows this texel covers: at least one (a smaller source is sampled nearest).
		const int64 Y0 = FMath::Min<int64>(static_cast<int64>(Y) * Height / Side, Height - 1);
		const int64 Y1 = FMath::Clamp<int64>(static_cast<int64>(Y + 1) * Height / Side, Y0 + 1, Height);
		for (int32 X = 0; X < Side; ++X)
		{
			const int64 X0 = FMath::Min<int64>(static_cast<int64>(X) * Width / Side, Width - 1);
			const int64 X1 = FMath::Clamp<int64>(static_cast<int64>(X + 1) * Width / Side, X0 + 1, Width);
			FLinearColor Sum(0.0f, 0.0f, 0.0f, 0.0f);
			for (int64 SY = Y0; SY < Y1; ++SY)
			{
				for (int64 SX = X0; SX < X1; ++SX)
				{
					Sum += Pixels[SY * Width + SX];
				}
			}
			const float Count = static_cast<float>((Y1 - Y0) * (X1 - X0));
			Out[Y * Side + X] = (Sum / Count).ToFColor(/*bSRGB*/ true);
		}
	}
	return Out;
}

UTexture2D* FMantleVoxelize::FindBaseColorTexture(UMaterialInterface* Material)
{
	// The upload's and the converter's own reading of a material's base colour (#620).
	return Material ? Cast<UTexture2D>(FMantleCosmeticExport::ResolveMaterialTextures(Material).BaseColor) : nullptr;
}

FLinearColor FMantleVoxelize::FindBaseColor(UMaterialInterface* Material)
{
	const FLinearColor Grey(0.5f, 0.5f, 0.5f, 1.0f);
	if (!Material)
	{
		return Grey;
	}
	// A colour parameter named for it ("BaseColor", "Albedo", "Diffuse", "Tint", "Color").
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Material->GetAllVectorParameterInfo(Infos, Ids);
	for (const TCHAR* Hint : { TEXT("base"), TEXT("albedo"), TEXT("diffuse"), TEXT("colo"), TEXT("tint") })
	{
		for (const FMaterialParameterInfo& Info : Infos)
		{
			FLinearColor Value;
			if (Info.Name.ToString().Contains(Hint) && Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), Value))
			{
				Value.A = 1.0f;
				return Value;
			}
		}
	}
	// A constant wired straight into Base Color.
	const UMaterial* Base = Material->GetMaterial();
	const UMaterialEditorOnlyData* EditorData = Base ? Base->GetEditorOnlyData() : nullptr;
	if (const UMaterialExpression* Expression = EditorData ? EditorData->BaseColor.Expression : nullptr)
	{
		if (const UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			return FLinearColor(Constant3->Constant.R, Constant3->Constant.G, Constant3->Constant.B, 1.0f);
		}
		if (const UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
		{
			return FLinearColor(Constant4->Constant.R, Constant4->Constant.G, Constant4->Constant.B, 1.0f);
		}
	}
	return Grey;
}

FMantleVoxelizeResult FMantleVoxelize::Voxelize(UWorld* World, const TArray<AActor*>& Sources, const FMantleVoxelizeOptions& Options)
{
	using namespace MantleVoxelizePrivate;
	FMantleVoxelizeResult Result;
	// The edge of one cell: a 100 cm block, or a 50 cm voxel of smooth ground.
	const double Cell = Options.bSmooth ? MantleVoxel::SmoothVoxelSize : MantleVoxel::BlockSize;
	if (!World)
	{
		Result.Error = TEXT("There is no level to voxelize in.");
		return Result;
	}

	// The fills and block types, from the source actors' meshes (never a block world itself, or meshes voxelized before).
	TArray<FFill> Fills;
	TArray<FSourceType> Types;
	TMap<TPair<UStaticMesh*, UMaterialInterface*>, int32> TypeOf;
	TMap<UStaticMesh*, TUniquePtr<FMeshShape>> Shapes;
	TArray<AActor*> Used;
	for (AActor* Actor : Sources)
	{
		if (!IsValid(Actor) || Actor->GetWorld() != World || Actor->IsA<AMantleVoxelWorldActor>() || Actor->ActorHasTag(AMantleVoxelWorldActor::VoxelizedSourceTag))
		{
			continue;
		}
		const int32 Before = Fills.Num();
		CollectFills(Actor, Used.Num(), Options.bSmooth, Fills, Types, TypeOf, Shapes);
		if (Fills.Num() > Before)
		{
			Used.Add(Actor);
		}
	}
	if (Fills.Num() == 0)
	{
		Result.Error = TEXT("None of the selected actors has a static mesh to turn into blocks. Select the cube meshes, then Voxelize again.");
		return Result;
	}
	if (Options.bSmooth && Types.Num() > FMantlePlaceLimits::MaxVoxelSmoothMaterials)
	{
		Result.Error = FString::Printf(TEXT("The selection uses %d different mesh and material pairs; smooth ground has at most %d materials. Voxelize fewer kinds of mesh at a time, or as blocks."),
			Types.Num(), FMantlePlaceLimits::MaxVoxelSmoothMaterials);
		return Result;
	}
	if (Types.Num() > FMantlePlaceLimits::MaxVoxelBlockTypes)
	{
		Result.Error = FString::Printf(TEXT("The selection uses %d different mesh and material pairs; a block world has at most %d block types. Voxelize fewer kinds of cube at a time."),
			Types.Num(), FMantlePlaceLimits::MaxVoxelBlockTypes);
		return Result;
	}

	// The cells every fill covers, and the world around them: from the lowest filled cell, rounded up to whole sections.
	struct FCells
	{
		int64 Lo[3];
		int64 Hi[3];
	};
	TArray<FCells> CellsOf;
	CellsOf.Reserve(Fills.Num());
	int64 Min[3] = { MAX_int64, MAX_int64, MAX_int64 };
	int64 Max[3] = { MIN_int64, MIN_int64, MIN_int64 };
	for (const FFill& Fill : Fills)
	{
		FCells& Cells = CellsOf.AddDefaulted_GetRef();
		bool bEmpty = false;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Fill.Shape)
			{
				// Every cell the box touches: a surface (a thin wall) can fill a cell whose centre is outside its bounds.
				Cells.Lo[Axis] = static_cast<int64>(FMath::Clamp(FMath::FloorToDouble(Fill.Box.Min[Axis] / Cell), -1.0e12, 1.0e12));
				Cells.Hi[Axis] = static_cast<int64>(FMath::Clamp(FMath::FloorToDouble(Fill.Box.Max[Axis] / Cell), -1.0e12, 1.0e12));
			}
			else
			{
				CellRange(Fill.Box.Min[Axis], Fill.Box.Max[Axis], Cells.Lo[Axis], Cells.Hi[Axis], Cell);
			}
			bEmpty |= Cells.Lo[Axis] > Cells.Hi[Axis];
		}
		if (bEmpty)
		{
			// Thinner than a block's centre on some axis (a flat card): it covers no cell, so none of its loops run.
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Cells.Lo[Axis] = 1;
				Cells.Hi[Axis] = 0;
			}
			continue;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Min[Axis] = FMath::Min(Min[Axis], Cells.Lo[Axis]);
			Max[Axis] = FMath::Max(Max[Axis], Cells.Hi[Axis]);
		}
	}
	if (Min[0] > Max[0])
	{
		Result.Error = FString::Printf(TEXT("The selected meshes are too thin to fill any %.0f cm cell: each cell's centre must lie inside a mesh's bounds."), Cell);
		return Result;
	}
	const int64 Caps[3] = { FMantlePlaceLimits::MaxVoxelWorldSizeXY, FMantlePlaceLimits::MaxVoxelWorldSizeXY, FMantlePlaceLimits::MaxVoxelWorldSizeZ };
	const int64 Step = FMantlePlaceLimits::VoxelSectionSize;
	int64 Extent[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Extent[Axis] = Max[Axis] - Min[Axis] + 1;
	}
	if (Extent[0] > Caps[0] || Extent[1] > Caps[1] || Extent[2] > Caps[2])
	{
		Result.Error = FString::Printf(TEXT("The selection spans %lld x %lld x %lld cells; a block world is at most %lld x %lld x %lld (%.0f cm cells). Voxelize a smaller part of the level."),
			Extent[0], Extent[1], Extent[2], Caps[0], Caps[1], Caps[2], Cell);
		return Result;
	}
	const FIntVector Size(
		static_cast<int32>((Extent[0] + Step - 1) / Step * Step),
		static_cast<int32>((Extent[1] + Step - 1) / Step * Step),
		static_cast<int32>((Extent[2] + Step - 1) / Step * Step));

	// The blocks, through the game's own grid, so the file is the one the game writes.
	TArray<FMantleVoxelBlockType> GridTypes;
	GridTypes.AddDefaulted(Types.Num() + 1);
	TSet<FString> Names;
	for (int32 Index = 0; Index < Types.Num(); ++Index)
	{
		const FString Name = MakeTypeName(Options.bSmooth && Types[Index].Material ? Types[Index].Material->GetName() : Types[Index].Mesh->GetName(), Names);
		Result.TypeNames.Add(Name);
		GridTypes[Index + 1].Name = FName(*Name);
	}
	FMantleVoxelGrid Grid;
	FString GridError;
	if (!Grid.Init(Size, MoveTemp(GridTypes), &GridError, Options.bSmooth))
	{
		Result.Error = FString::Printf(TEXT("The block world could not be made: %s."), *GridError);
		return Result;
	}
	// Blocks each source actor made solid, to name one that fills most of the world (a sky dome, #64).
	TArray<int64> SourceBlocks;
	SourceBlocks.SetNumZeroed(Used.Num());
	for (int32 FillIndex = 0; FillIndex < Fills.Num(); ++FillIndex)
	{
		const FCells& Cells = CellsOf[FillIndex];
		const uint8 Id = static_cast<uint8>(Fills[FillIndex].Type + 1);
		int64& Counted = SourceBlocks[Fills[FillIndex].Source];
		bool bOverCap = false;
		FillCells(Fills[FillIndex], Cells.Lo, Cells.Hi, Cell, [&](int64 X, int64 Y, int64 Z)
		{
			const FIntVector P(static_cast<int32>(X - Min[0]), static_cast<int32>(Y - Min[1]), static_cast<int32>(Z - Min[2]));
			if (bOverCap || Grid.GetBlock(P) == Id)
			{
				return;
			}
			const FMantleVoxelGrid::ESetResult Set = Options.bSmooth ? Grid.SetVoxel(P, Id, MantleVoxel::FullDensity) : Grid.SetBlock(P, Id);
			if (Set == FMantleVoxelGrid::ESetResult::OverCap)
			{
				bOverCap = true;
				return;
			}
			++Counted;
		});
		if (bOverCap)
		{
			Result.Error = FString::Printf(TEXT("The selection has too many mixed 16-cell sections for one block world (at most %d). Voxelize a smaller part of the level."),
				Options.bSmooth ? MantleVoxel::MaxNonUniformSmoothSections : MantleVoxel::MaxNonUniformSections);
			return Result;
		}
	}
	for (int32 Section = 0; Section < Grid.NumSections(); ++Section)
	{
		Grid.CompactSection(Section);
	}
	const FMantleVoxelWorldFile File = FMantleVoxelWorldFile::FromGrid(Grid);

	// The tiles, one per block type, before the level changes.
	TArray<FMantleVoxelBlockTypeSettings> BlockTypes;
	for (int32 Index = 0; Index < Types.Num(); ++Index)
	{
		FString TileError;
		UTexture2D* Tile = MakeTile(Options.TileFolder, FString::Printf(TEXT("T_%s"), *Result.TypeNames[Index]), Options.TileSize,
			TilePixels(Types[Index].Material, Options.TileSize), TileError);
		if (!Tile)
		{
			Result.Error = FString::Printf(TEXT("Block type '%s': %s."), *Result.TypeNames[Index], *TileError);
			return Result;
		}
		FMantleVoxelBlockTypeSettings& Type = BlockTypes.AddDefaulted_GetRef();
		Type.Name = Result.TypeNames[Index];
		Type.Top = Tile;
		Type.Side = Tile;
		Type.Bottom = Tile;
		Type.Render = RenderFor(Types[Index].Material);
	}

	// The world at the lowest filled cell, and the sources out of the way: in their folder, hidden, left out of the place.
	Result.Origin = FVector(static_cast<double>(Min[0]), static_cast<double>(Min[1]), static_cast<double>(Min[2])) * Cell;
	AMantleVoxelWorldActor* Actor = SpawnWorld(World, Result.Origin);
	if (!Actor)
	{
		Result.Error = TEXT("The block world could not be added to this level.");
		return Result;
	}
	Actor->TileSize = Options.TileSize;
	Actor->BlockTypes = MoveTemp(BlockTypes);
	Actor->SetBlocks(File);
	for (int32 Source = 0; Source < Used.Num(); ++Source)
	{
		if (SourceBlocks[Source] > Actor->LargestSourceBlocks)
		{
			Actor->LargestSourceBlocks = SourceBlocks[Source];
			Actor->LargestSource = Used[Source]->GetActorLabel();
		}
	}
	Result.Warning = Actor->DescribeLargestSource();
	for (AActor* Source : Used)
	{
		Source->Modify();
		Source->Tags.AddUnique(AMantleVoxelWorldActor::VoxelizedSourceTag);
		Source->SetFolderPath(AMantleVoxelWorldActor::VoxelizedSourceFolder);
		// Hidden by its components' visibility, which Undo puts back with the rest.
		TInlineComponentArray<UPrimitiveComponent*> Primitives;
		Source->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			Primitive->Modify();
			Primitive->SetVisibility(false);
		}
	}
	Result.World = Actor;
	Result.Size = Actor->Size;
	Result.Blocks = Actor->BlockCount;
	Result.SourceActors = Used.Num();
	return Result;
}

FMantleVoxelizeResult FMantleVoxelize::ImportMbw(UWorld* World, TConstArrayView<uint8> Bytes, const FVector& Location, const FMantleVoxelizeOptions& Options)
{
	using namespace MantleVoxelizePrivate;
	FMantleVoxelizeResult Result;
	if (!World)
	{
		Result.Error = TEXT("There is no level to import into.");
		return Result;
	}
	FMantleVoxelWorldFile File;
	FString ReadError;
	if (!FMantleVoxelWorldFile::Read(Bytes, File, &ReadError))
	{
		Result.Error = FString::Printf(TEXT("The file %s."), *ReadError);
		return Result;
	}

	// Placeholder block types for every id the file uses (at least one: a world needs a block type).
	int32 MaxId = 0;
	uint8 Ids[MantleVoxel::SectionVolume];
	for (const FMantleVoxelWorldFile::FSection& Section : File.Sections)
	{
		if (Section.Blocks.IsUniform())
		{
			MaxId = FMath::Max<int32>(MaxId, Section.Blocks.GetUniformId());
			continue;
		}
		Section.Blocks.GetAll(Ids);
		for (const uint8 Id : Ids)
		{
			MaxId = FMath::Max<int32>(MaxId, Id);
		}
	}
	const int32 TypeCount = FMath::Max(MaxId, 1);
	TArray<FColor> Grey;
	Grey.Init(FColor(128, 128, 128, 255), Options.TileSize * Options.TileSize);
	FString TileError;
	UTexture2D* Tile = MakeTile(Options.TileFolder, TEXT("T_placeholder"), Options.TileSize, Grey, TileError);
	if (!Tile)
	{
		Result.Error = FString::Printf(TEXT("The placeholder tile: %s."), *TileError);
		return Result;
	}

	AMantleVoxelWorldActor* Actor = SpawnWorld(World, Location);
	if (!Actor)
	{
		Result.Error = TEXT("The block world could not be added to this level.");
		return Result;
	}
	Actor->TileSize = Options.TileSize;
	for (int32 Id = 1; Id <= TypeCount; ++Id)
	{
		FMantleVoxelBlockTypeSettings& Type = Actor->BlockTypes.AddDefaulted_GetRef();
		Type.Name = FString::Printf(TEXT("block_%d"), Id);
		Type.Top = Tile;
		Type.Side = Tile;
		Type.Bottom = Tile;
		Result.TypeNames.Add(Type.Name);
	}
	Actor->SetBlocks(File);
	Result.World = Actor;
	Result.Origin = Location;
	Result.Size = Actor->Size;
	Result.Blocks = Actor->BlockCount;
	return Result;
}

void FMantleVoxelize::VoxelizeSelection(const bool bSmooth)
{
	using namespace MantleVoxelizePrivate;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TArray<AActor*> Selected;
	if (GEditor && GEditor->GetSelectedActors())
	{
		GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Selected);
	}
	if (!World || Selected.Num() == 0)
	{
		Notify(LOCTEXT("VoxelizeNothing", "Nothing to voxelize"), TEXT("Select the cube meshes to turn into blocks, then choose Voxelize again."), false);
		return;
	}
	FScopedTransaction Transaction(LOCTEXT("VoxelizeUndo", "Voxelize"));
	FMantleVoxelizeOptions Options;
	Options.TileFolder = TileFolderFor(World);
	Options.bSmooth = bSmooth;
	const FMantleVoxelizeResult Result = Voxelize(World, Selected, Options);
	if (!Result.IsOk())
	{
		Transaction.Cancel();
		Notify(LOCTEXT("VoxelizeFailed", "Could not voxelize"), Result.Error, false);
		return;
	}
	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	GEditor->SelectActor(Result.World, /*bInSelected*/ true, /*bNotify*/ true);
	Notify(FText::Format(LOCTEXT("Voxelized", "Voxelized {0} actors into a block world"), FText::AsNumber(Result.SourceActors)),
		FString::Printf(TEXT("%lld blocks of %d types (%s), %d x %d x %d blocks. The meshes are hidden in the \"%s\" folder and left out of the place; tiles are in %s."),
			Result.Blocks, Result.TypeNames.Num(), *DescribeTypes(Result.TypeNames), Result.Size.X, Result.Size.Y, Result.Size.Z,
			*AMantleVoxelWorldActor::VoxelizedSourceFolder.ToString(), *Options.TileFolder)
			+ (Result.Warning.IsEmpty() ? FString() : FString::Printf(TEXT(" Check: the world %s."), *Result.Warning)),
		Result.Warning.IsEmpty());
}

void FMantleVoxelize::ImportWithDialog()
{
	using namespace MantleVoxelizePrivate;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!World || !DesktopPlatform)
	{
		Notify(LOCTEXT("ImportNoPicker", "Could not import a block world"), TEXT("Open a level first; this editor also needs a file picker."), false);
		return;
	}
	const void* ParentWindow = FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
	TArray<FString> Files;
	if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("Pick a block world (.mbw)"), FString(), FString(), TEXT("Block world (*.mbw)|*.mbw"), EFileDialogFlags::None, Files)
		|| Files.IsEmpty())
	{
		// Cancelled: nothing changes and there is nothing to say.
		return;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Files[0]))
	{
		Notify(LOCTEXT("ImportUnreadable", "Could not import a block world"), FString::Printf(TEXT("%s could not be read."), *Files[0]), false);
		return;
	}
	FScopedTransaction Transaction(LOCTEXT("ImportUndo", "Import Block World"));
	FMantleVoxelizeOptions Options;
	Options.TileFolder = TileFolderFor(World);
	const FMantleVoxelizeResult Result = ImportMbw(World, Bytes, ImportLocation(), Options);
	if (!Result.IsOk())
	{
		Transaction.Cancel();
		Notify(LOCTEXT("ImportFailed", "Could not import a block world"), FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(Files[0]), *Result.Error), false);
		return;
	}
	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	GEditor->SelectActor(Result.World, /*bInSelected*/ true, /*bNotify*/ true);
	Notify(LOCTEXT("Imported", "Imported a block world"),
		FString::Printf(TEXT("%lld blocks, %d x %d x %d blocks, with %d placeholder block types (block_1 to block_%d). Give each its name and tiles in the Details panel."),
			Result.Blocks, Result.Size.X, Result.Size.Y, Result.Size.Z, Result.TypeNames.Num(), Result.TypeNames.Num()),
		true);
}

#undef LOCTEXT_NAMESPACE
