// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelWorldActor.h"

#include "MantlePlaceLimits.h"
#include "MantleSurface.h"
#include "MantleVoxelTypes.h"
#include "MantleVoxelWorldFile.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Texture2D.h"

// Each block surface converts to EMantleSurface by its number (the editor enum lists them in its order).
static_assert(MantleSurfaceCount == static_cast<int32>(EMantleVoxelBlockSurface::Leaf) + 1, "EMantleVoxelBlockSurface mirrors EMantleSurface");

const FName AMantleVoxelWorldActor::VoxelizedSourceTag(TEXT("MantleVoxelizedSource"));
const FName AMantleVoxelWorldActor::VoxelizedSourceFolder(TEXT("Voxelized sources"));

AMantleVoxelWorldActor::AMantleVoxelWorldActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// A plain root: the entity's location is the world's lowest corner, and nothing of the actor is drawn in the game.
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Static);
	SetRootComponent(Root);

#if WITH_EDITORONLY_DATA
	// The world's outline, in the editor only (an editor-only, visualisation component the exporter never reads).
	Bounds = CreateEditorOnlyDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	if (Bounds)
	{
		Bounds->SetupAttachment(Root);
		Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Bounds->SetCanEverAffectNavigation(false);
		Bounds->SetGenerateOverlapEvents(false);
		Bounds->bHiddenInGame = true;
		Bounds->ShapeColor = FColor(80, 200, 255);
		Bounds->SetLineThickness(4.0f);
	}
#endif
}

void AMantleVoxelWorldActor::SetBlocks(const FMantleVoxelWorldFile& File)
{
	File.Write(WorldData);
	Size = File.Size;
	BlockCount = 0;
	const TArray<int64> Counts = CountBlocks();
	for (int32 Id = 1; Id < Counts.Num(); ++Id)
	{
		BlockCount += Counts[Id];
	}
	UpdateBounds();
}

FString AMantleVoxelWorldActor::WorldKey() const
{
	return FString::Printf(TEXT("voxelworld:%s"), *GetPathName());
}

FString AMantleVoxelWorldActor::DescribeLargestSource() const
{
	constexpr int64 SectionBlocks = 16 * 16 * 16;
	if (LargestSource.IsEmpty() || BlockCount < SectionBlocks || LargestSourceBlocks * 2 <= BlockCount)
	{
		return FString();
	}
	return FString::Printf(TEXT("has %lld of its %lld blocks (%.0f%%) from one voxelized mesh, '%s'; a sky dome, ground plane or backdrop turns into a solid mass of blocks"),
		LargestSourceBlocks, BlockCount, 100.0 * static_cast<double>(LargestSourceBlocks) / static_cast<double>(BlockCount), *LargestSource);
}

TArray<int64> AMantleVoxelWorldActor::CountBlocks() const
{
	TArray<int64> Counts;
	FMantleVoxelWorldFile File;
	if (WorldData.Num() == 0 || !FMantleVoxelWorldFile::Read(WorldData, File))
	{
		return Counts;
	}
	Counts.SetNumZeroed(256);
	uint8 Ids[MantleVoxel::SectionVolume];
	for (const FMantleVoxelWorldFile::FSection& Section : File.Sections)
	{
		if (Section.Blocks.IsUniform())
		{
			Counts[Section.Blocks.GetUniformId()] += MantleVoxel::SectionVolume;
			continue;
		}
		Section.Blocks.GetAll(Ids);
		for (const uint8 Id : Ids)
		{
			++Counts[Id];
		}
	}
	return Counts;
}

TArray<FMantleVoxelWorldProblem> AMantleVoxelWorldActor::DescribeProblems() const
{
	TArray<FMantleVoxelWorldProblem> Out;
	auto Add = [&Out](const FString& What, const FString& Remedy) { Out.Add(FMantleVoxelWorldProblem{ What, Remedy }); };

	const int32 Step = FMantlePlaceLimits::VoxelSectionSize;
	const int32 Max[3] = { FMantlePlaceLimits::MaxVoxelWorldSizeXY, FMantlePlaceLimits::MaxVoxelWorldSizeXY, FMantlePlaceLimits::MaxVoxelWorldSizeZ };
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Size[Axis] < Step || Size[Axis] > Max[Axis] || Size[Axis] % Step != 0)
		{
			Add(FString::Printf(TEXT("is %d x %d x %d blocks; each side must be a multiple of %d, at most %d x %d x %d."), Size.X, Size.Y, Size.Z, Step,
				FMantlePlaceLimits::MaxVoxelWorldSizeXY, FMantlePlaceLimits::MaxVoxelWorldSizeXY, FMantlePlaceLimits::MaxVoxelWorldSizeZ),
				TEXT("Voxelize a smaller part of the level, or split it into more than one place."));
			break;
		}
	}

	// The blocks: a file that reads, of the world's size, naming only listed block types.
	if (WorldData.Num() > 0)
	{
		FMantleVoxelWorldFile File;
		FString Error;
		if (!FMantleVoxelWorldFile::Read(WorldData, File, &Error))
		{
			Add(FString::Printf(TEXT("has blocks that cannot be read: the world file %s."), *Error), TEXT("Voxelize or import the blocks again."));
		}
		else if (File.Size != Size)
		{
			Add(FString::Printf(TEXT("holds %s blocks but says it is %s."), *File.Size.ToString(), *Size.ToString()), TEXT("Voxelize or import the blocks again."));
		}
		else
		{
			const TArray<int64> Counts = CountBlocks();
			for (int32 Id = Counts.Num() - 1; Id > BlockTypes.Num(); --Id)
			{
				if (Counts[Id] > 0)
				{
					Add(FString::Printf(TEXT("has blocks of id %d but only %d block types, so those blocks would be air."), Id, BlockTypes.Num()),
						TEXT("Add block types to the list until every id has one."));
					break;
				}
			}
		}
	}

	if (BlockTypes.Num() < 1 || BlockTypes.Num() > FMantlePlaceLimits::MaxVoxelBlockTypes)
	{
		Add(FString::Printf(TEXT("has %d block types; a block world has 1 to %d."), BlockTypes.Num(), FMantlePlaceLimits::MaxVoxelBlockTypes),
			BlockTypes.Num() < 1 ? TEXT("Add a block type.") : TEXT("Use fewer different meshes and materials."));
	}

	if (TileSize != 16 && TileSize != 32 && TileSize != 64 && TileSize != 128)
	{
		Add(FString::Printf(TEXT("has a Tile Size of %d; it must be 16, 32, 64 or 128."), TileSize), TEXT("Pick one of those in the Details panel."));
	}

	TSet<FString> Names;
	TSet<const UTexture2D*> Tiles;
	for (int32 Index = 0; Index < BlockTypes.Num(); ++Index)
	{
		const FMantleVoxelBlockTypeSettings& Type = BlockTypes[Index];
		const FString Which = FString::Printf(TEXT("Block type %d ('%s')"), Index + 1, *Type.Name);
		bool bChars = !Type.Name.IsEmpty() && Type.Name.Len() <= FMantlePlaceLimits::MaxVoxelBlockNameLength;
		for (const TCHAR C : Type.Name)
		{
			bChars &= (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_';
		}
		if (!bChars || Type.Name == TEXT("air"))
		{
			Add(FString::Printf(TEXT("%s needs a name of 1 to %d of a-z, 0-9 and _, and not 'air'."), *Which, FMantlePlaceLimits::MaxVoxelBlockNameLength),
				TEXT("Rename it in the Details panel."));
		}
		else
		{
			bool bAlready = false;
			Names.Add(Type.Name, &bAlready);
			if (bAlready)
			{
				Add(FString::Printf(TEXT("%s has the same name as an earlier block type."), *Which), TEXT("Give each block type its own name."));
			}
		}
		if (Type.Hardness != -1.0f && (Type.Hardness < 0.0f || Type.Hardness > FMantlePlaceLimits::MaxVoxelBlockHardness))
		{
			Add(FString::Printf(TEXT("%s has a hardness of %g; it must be -1 (unbreakable) or 0 to %g seconds."), *Which, Type.Hardness, FMantlePlaceLimits::MaxVoxelBlockHardness),
				TEXT("Change it in the Details panel."));
		}
		const TPair<const TCHAR*, const UTexture2D*> Faces[] = { { TEXT("top"), Type.Top.Get() }, { TEXT("side"), Type.Side.Get() }, { TEXT("bottom"), Type.Bottom.Get() } };
		for (const TPair<const TCHAR*, const UTexture2D*>& Face : Faces)
		{
			if (!Face.Value)
			{
				Add(FString::Printf(TEXT("%s has no %s tile."), *Which, Face.Key), TEXT("Pick a texture for it in the Details panel."));
				continue;
			}
			Tiles.Add(Face.Value);
			// The tile uploads from its source pixels, so the source is what must be square.
			const int32 Width = Face.Value->Source.IsValid() ? Face.Value->Source.GetSizeX() : 0;
			const int32 Height = Face.Value->Source.IsValid() ? Face.Value->Source.GetSizeY() : 0;
			if (Width != TileSize || Height != TileSize)
			{
				Add(FString::Printf(TEXT("%s's %s tile '%s' is %d x %d pixels; every tile of this world is %d x %d."), *Which, Face.Key, *Face.Value->GetName(),
					Width, Height, TileSize, TileSize),
					TEXT("Use a texture of exactly the Tile Size (Voxelize makes them), or change the Tile Size."));
			}
		}
	}
	if (Tiles.Num() > FMantlePlaceLimits::MaxVoxelTileTextures)
	{
		Add(FString::Printf(TEXT("uses %d different tile textures; a block world holds at most %d."), Tiles.Num(), FMantlePlaceLimits::MaxVoxelTileTextures),
			TEXT("Share tiles between block types."));
	}

	// The world's voxels are a fixed grid: the entity has a location only.
	const FTransform Transform = GetActorTransform();
	if (!Transform.GetRotation().Rotator().IsNearlyZero(0.001) || !Transform.GetScale3D().Equals(FVector::OneVector, 0.0001))
	{
		Add(TEXT("is rotated or scaled; a block world is placed by its location only."), TEXT("Set its rotation to 0, 0, 0 and its scale to 1, 1, 1."));
	}
	return Out;
}

void AMantleVoxelWorldActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateBounds();
}

#if WITH_EDITOR
void AMantleVoxelWorldActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateBounds();
}
#endif

void AMantleVoxelWorldActor::UpdateBounds()
{
	// The box from the lowest corner (the actor's location) out to the far corner.
	if (Bounds)
	{
		const FVector Extent = FVector(Size) * (MantleVoxel::BlockSize * 0.5);
		Bounds->SetBoxExtent(Extent, /*bUpdateOverlaps*/ false);
		Bounds->SetRelativeLocation(Extent);
	}
}
