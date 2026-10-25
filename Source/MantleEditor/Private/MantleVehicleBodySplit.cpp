// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBodySplit.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "MantleSkinnedMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

// Named, not anonymous: unity builds merge this file with others.
namespace MantleVehicleBodySplitPrivate
{
	/** The bone a vertex rides: its first influence (a body from a model has one, whole). */
	int32 BoneOfVertex(const FMantleSkinnedMeshLOD& LOD, const int32 Vertex)
	{
		const int32 Stride = FMath::Max(1, LOD.NumInfluences);
		const int32 At = Vertex * Stride;
		return LOD.InfluenceBones.IsValidIndex(At) ? static_cast<int32>(LOD.InfluenceBones[At]) : 0;
	}

	/** A section that draws something. */
	bool Draws(const FMantleSkinnedMeshSection& Section, const FMantleSkinnedMeshLOD& LOD)
	{
		return Section.IndexCount >= 3 && Section.FirstIndex >= 0 && Section.FirstIndex + Section.IndexCount <= LOD.Indices.Num();
	}

	/** Each bone's place in the body's space, from the local reference pose (parents first). */
	TArray<FTransform> ComponentPose(const FMantleSkinnedMesh& Body)
	{
		TArray<FTransform> Pose;
		Pose.SetNum(Body.Bones.Num());
		for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
		{
			const FTransform Local = Body.ReferencePose.IsValidIndex(Bone) ? Body.ReferencePose[Bone] : FTransform::Identity;
			const int32 Parent = Body.Bones[Bone].ParentIndex;
			Pose[Bone] = Parent >= 0 && Parent < Bone ? Local * Pose[Parent] : Local;
		}
		return Pose;
	}

	/** The saved-asset side: a package's asset already there, loaded or on disk, or null. */
	UStaticMesh* FindExistingMesh(const FString& PackageName, const FString& AssetName)
	{
		if (UPackage* Loaded = FindPackage(nullptr, *PackageName))
		{
			if (UStaticMesh* InMemory = FindObject<UStaticMesh>(Loaded, *AssetName))
			{
				return InMemory;
			}
		}
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			return nullptr;
		}
		return LoadObject<UStaticMesh>(nullptr, *(PackageName + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
}

int32 FMantleVehicleBodySplit::CountMaterials(const FMantleSkinnedMesh& Body)
{
	using namespace MantleVehicleBodySplitPrivate;
	if (Body.LODs.Num() == 0)
	{
		return 0;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	if (LOD.Sections.Num() == 0)
	{
		return LOD.Indices.Num() >= 3 ? 1 : 0;
	}
	TSet<int32> Used;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		if (Draws(Section, LOD))
		{
			Used.Add(Section.MaterialIndex);
		}
	}
	return Used.Num();
}

bool FMantleVehicleBodySplit::Split(FMantleSkinnedMesh& Body, TArray<FSkeletalMaterial>& Materials, FMantleVehicleBodyExtras& Extras, const int32 MaxBodyMaterials,
	const int32 MaxPieceMaterials, const int32 MaxPieces, FMantleVehicleBodySplitResult& OutResult)
{
	using namespace MantleVehicleBodySplitPrivate;
	OutResult = FMantleVehicleBodySplitResult();
	OutResult.MaterialsBefore = CountMaterials(Body);
	OutResult.MaterialsAfter = OutResult.MaterialsBefore;
	if (Body.LODs.Num() == 0 || OutResult.MaterialsBefore <= MaxBodyMaterials || MaxBodyMaterials < 1 || MaxPieceMaterials < 1)
	{
		return true;
	}
	FMantleSkinnedMeshLOD& LOD = Body.LODs[0];

	// The body keeps the materials with the most triangles (the look of the vehicle); the others are split off.
	TMap<int32, int32> Triangles;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		if (Draws(Section, LOD))
		{
			Triangles.FindOrAdd(Section.MaterialIndex) += Section.IndexCount / 3;
		}
	}
	TArray<int32> ByWeight;
	Triangles.GetKeys(ByWeight);
	ByWeight.Sort([&Triangles](const int32 A, const int32 B) { return Triangles[A] != Triangles[B] ? Triangles[A] > Triangles[B] : A < B; });
	TSet<int32> Kept;
	for (int32 Index = 0; Index < MaxBodyMaterials; ++Index)
	{
		Kept.Add(ByWeight[Index]);
	}

	// The moved materials by the bone their triangles ride.
	TMap<int32, TArray<int32>> MovedOnBone;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		if (!Draws(Section, LOD) || Kept.Contains(Section.MaterialIndex))
		{
			continue;
		}
		for (int32 At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount; At += 3)
		{
			MovedOnBone.FindOrAdd(BoneOfVertex(LOD, static_cast<int32>(LOD.Indices[At]))).AddUnique(Section.MaterialIndex);
		}
	}
	TArray<int32> Bones;
	MovedOnBone.GetKeys(Bones);
	Bones.Sort();
	int32 PieceCount = 0;
	for (const int32 Bone : Bones)
	{
		MovedOnBone[Bone].Sort();
		PieceCount += FMath::DivideAndRoundUp(MovedOnBone[Bone].Num(), MaxPieceMaterials);
	}
	if (PieceCount > MaxPieces)
	{
		OutResult.Problem = FString::Printf(TEXT("it would take %d extra parts to carry its %d materials, and a vehicle has at most %d parts"), PieceCount, OutResult.MaterialsBefore, MaxPieces);
		return false;
	}

	// The pieces.
	const TArray<FTransform> Pose = ComponentPose(Body);
	const bool bTangents = LOD.Tangents.Num() == LOD.Positions.Num();
	for (const int32 Bone : Bones)
	{
		const TArray<int32>& OnBone = MovedOnBone[Bone];
		for (int32 First = 0; First < OnBone.Num(); First += MaxPieceMaterials)
		{
			FMantleVehicleBodyPiece& Piece = OutResult.Pieces.AddDefaulted_GetRef();
			Piece.BoneIndex = Bone;
			Piece.Bone = Body.Bones.IsValidIndex(Bone) ? Body.Bones[Bone].Name : FName();
			Piece.ToBone = Pose.IsValidIndex(Bone) ? Pose[Bone].Inverse() : FTransform::Identity;
			TMap<uint32, uint32> NewVertex;
			for (int32 Slot = First; Slot < OnBone.Num() && Slot < First + MaxPieceMaterials; ++Slot)
			{
				const int32 Material = OnBone[Slot];
				FMantleVehicleBodyPiece::FSection& Added = Piece.Sections.AddDefaulted_GetRef();
				Added.FirstIndex = Piece.Indices.Num();
				Added.Material = Piece.Materials.Num();
				Piece.Materials.Add(Materials.IsValidIndex(Material) ? Materials[Material].MaterialInterface.Get() : nullptr);
				FName SlotName = Materials.IsValidIndex(Material) ? Materials[Material].MaterialSlotName : NAME_None;
				if (SlotName.IsNone() || Piece.SlotNames.Contains(SlotName))
				{
					SlotName = FName(*FString::Printf(TEXT("material_%d"), Material));
				}
				Piece.SlotNames.Add(SlotName);
				for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
				{
					if (!Draws(Section, LOD) || Section.MaterialIndex != Material)
					{
						continue;
					}
					for (int32 At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount; At += 3)
					{
						if (BoneOfVertex(LOD, static_cast<int32>(LOD.Indices[At])) != Bone)
						{
							continue;
						}
						for (int32 Corner = 0; Corner < 3; ++Corner)
						{
							const uint32 Old = LOD.Indices[At + Corner];
							uint32* Found = NewVertex.Find(Old);
							if (!Found)
							{
								Found = &NewVertex.Add(Old, static_cast<uint32>(Piece.Positions.Num()));
								Piece.Positions.Add(LOD.Positions[Old]);
								Piece.Normals.Add(LOD.Normals.IsValidIndex(Old) ? LOD.Normals[Old] : FVector3f::UpVector);
								Piece.UVs.Add(LOD.UVs.IsValidIndex(Old) ? LOD.UVs[Old] : FVector2f::ZeroVector);
								if (bTangents)
								{
									Piece.Tangents.Add(LOD.Tangents[Old]);
								}
							}
							Piece.Indices.Add(*Found);
						}
					}
				}
				Added.IndexCount = Piece.Indices.Num() - Added.FirstIndex;
			}
			OutResult.MovedTriangles += Piece.NumTriangles();
		}
	}

	// The body without them: the kept sections in their order, over only the vertices they use, and only the material slots they draw.
	const int32 OldVertices = LOD.Positions.Num();
	TArray<int32> KeptSlots(Kept.Array());
	KeptSlots.Sort();
	TMap<int32, int32> NewSlot;
	for (const int32 Slot : KeptSlots)
	{
		NewSlot.Add(Slot, NewSlot.Num());
	}
	const int32 Stride = FMath::Max(1, LOD.NumInfluences);
	const bool bHair = LOD.HairCards.Num() == OldVertices;
	const bool bWeights = LOD.InfluenceBones.Num() == OldVertices * Stride && LOD.InfluenceWeights.Num() == OldVertices * Stride;
	TArray<int32> OldOfNew;
	TMap<uint32, uint32> NewOfOld;
	TArray<uint32> Indices;
	TArray<FMantleSkinnedMeshSection> Sections;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		if (!Draws(Section, LOD) || !Kept.Contains(Section.MaterialIndex))
		{
			continue;
		}
		FMantleSkinnedMeshSection& Added = Sections.Add_GetRef(Section);
		Added.FirstIndex = Indices.Num();
		Added.MaterialIndex = NewSlot[Section.MaterialIndex];
		for (int32 At = Section.FirstIndex; At < Section.FirstIndex + Section.IndexCount; ++At)
		{
			const uint32 Old = LOD.Indices[At];
			uint32* Found = NewOfOld.Find(Old);
			if (!Found)
			{
				Found = &NewOfOld.Add(Old, static_cast<uint32>(OldOfNew.Num()));
				OldOfNew.Add(static_cast<int32>(Old));
			}
			Indices.Add(*Found);
		}
	}
	const auto Gather = [&OldOfNew](const auto& From, auto& To, const int32 Per)
	{
		To.Reset();
		To.Reserve(OldOfNew.Num() * Per);
		for (const int32 Old : OldOfNew)
		{
			for (int32 Slot = 0; Slot < Per; ++Slot)
			{
				To.Add(From[Old * Per + Slot]);
			}
		}
	};
	const bool bExtraColours = Extras.Colors.Num() == OldVertices;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector4f> Tangents;
	TArray<FVector2f> UVs;
	TArray<FVector3f> Hair;
	TArray<uint16> InfluenceBones;
	TArray<uint8> InfluenceWeights;
	Gather(LOD.Positions, Positions, 1);
	Gather(LOD.Normals, Normals, 1);
	Gather(LOD.UVs, UVs, 1);
	if (bTangents)
	{
		Gather(LOD.Tangents, Tangents, 1);
	}
	if (bHair)
	{
		Gather(LOD.HairCards, Hair, 1);
	}
	if (bWeights)
	{
		Gather(LOD.InfluenceBones, InfluenceBones, Stride);
		Gather(LOD.InfluenceWeights, InfluenceWeights, Stride);
	}
	for (TArray<FVector2f>& Channel : Extras.UVs)
	{
		TArray<FVector2f> Gathered;
		if (Channel.Num() == OldVertices)
		{
			Gather(Channel, Gathered, 1);
		}
		Channel = MoveTemp(Gathered);
	}
	if (bExtraColours)
	{
		TArray<FColor> Gathered;
		Gather(Extras.Colors, Gathered, 1);
		Extras.Colors = MoveTemp(Gathered);
	}
	else
	{
		Extras.Colors.Reset();
	}
	LOD.Positions = MoveTemp(Positions);
	LOD.Normals = MoveTemp(Normals);
	LOD.Tangents = MoveTemp(Tangents);
	LOD.UVs = MoveTemp(UVs);
	LOD.HairCards = MoveTemp(Hair);
	LOD.InfluenceBones = MoveTemp(InfluenceBones);
	LOD.InfluenceWeights = MoveTemp(InfluenceWeights);
	LOD.Indices = MoveTemp(Indices);
	LOD.Sections = MoveTemp(Sections);

	TArray<FSkeletalMaterial> KeptMaterials;
	for (const int32 Slot : KeptSlots)
	{
		KeptMaterials.Add(Materials.IsValidIndex(Slot) ? Materials[Slot] : FSkeletalMaterial());
	}
	Materials = MoveTemp(KeptMaterials);
	OutResult.MaterialsAfter = CountMaterials(Body);
	return true;
}

UStaticMesh* FMantleVehicleBodySplit::MakePieceMesh(const FMantleVehicleBodyPiece& Piece, const FString& PackagePath, const FString& PieceName, FString& OutError)
{
	using namespace MantleVehicleBodySplitPrivate;
	OutError.Reset();
	if (Piece.Positions.Num() == 0 || Piece.Indices.Num() < 3 || Piece.Sections.Num() == 0)
	{
		OutError = TEXT("the part has no triangles");
		return nullptr;
	}
	const bool bPersist = !FMantleVehicleChassisMesh::IsScratchPath(PackagePath);
	const FString PackageName = PackagePath / PieceName;
	if (!PackagePath.StartsWith(TEXT("/")) || PieceName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName, !bPersist))
	{
		OutError = FString::Printf(TEXT("'%s' is not a folder a vehicle can be saved in"), *PackagePath);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("%s could not be created"), *PackageName);
		return nullptr;
	}
	UStaticMesh* Mesh = FindExistingMesh(PackageName, PieceName);
	const bool bNew = Mesh == nullptr;
	if (bNew)
	{
		Mesh = NewObject<UStaticMesh>(Package, FName(*PieceName), RF_Public | RF_Standalone);
	}
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("%s could not be created"), *PackageName);
		return nullptr;
	}
	Mesh->Modify();
	Mesh->GetStaticMaterials().Reset();
	for (int32 Slot = 0; Slot < Piece.Materials.Num(); ++Slot)
	{
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Piece.Materials[Slot], Piece.SlotNames[Slot], Piece.SlotNames[Slot]));
	}
	Mesh->GetSectionInfoMap().Clear();
	Mesh->GetOriginalSectionInfoMap().Clear();
	Mesh->bAllowCPUAccess = true;
	Mesh->SetNumSourceModels(1);

	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> GroupNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(1);
	const bool bTangents = Piece.Tangents.Num() == Piece.Positions.Num();
	TArray<FVertexInstanceID> Instances;
	Instances.Reserve(Piece.Positions.Num());
	for (int32 Vertex = 0; Vertex < Piece.Positions.Num(); ++Vertex)
	{
		const FVertexID Made = Description.CreateVertex();
		Positions[Made] = Piece.Positions[Vertex];
		const FVertexInstanceID Instance = Description.CreateVertexInstance(Made);
		Normals[Instance] = Piece.Normals[Vertex];
		if (bTangents)
		{
			const FVector4f& Tangent = Piece.Tangents[Vertex];
			Tangents[Instance] = FVector3f(Tangent.X, Tangent.Y, Tangent.Z);
			BinormalSigns[Instance] = Tangent.W < 0.0f ? -1.0f : 1.0f;
		}
		else
		{
			BinormalSigns[Instance] = 1.0f;
		}
		UVs.Set(Instance, 0, Piece.UVs[Vertex]);
		Instances.Add(Instance);
	}
	for (const FMantleVehicleBodyPiece::FSection& Section : Piece.Sections)
	{
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		GroupNames[Group] = Piece.SlotNames[Section.Material];
		for (int32 At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount; At += 3)
		{
			const FVertexInstanceID Corners[3] = { Instances[Piece.Indices[At]], Instances[Piece.Indices[At + 1]], Instances[Piece.Indices[At + 2]] };
			Description.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(Corners, 3));
		}
	}

	// The geometry is final: nothing recomputed but the tangents of a body that brought none, and no lightmap, distance field or Lumen cards.
	FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
	Settings.bRecomputeNormals = false;
	Settings.bRecomputeTangents = !bTangents;
	Settings.bGenerateLightmapUVs = false;
	Settings.bBuildReversedIndexBuffer = false;
	Settings.DistanceFieldResolutionScale = 0.0f;
	Settings.MaxLumenMeshCards = 0;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = true;
	TArray<const FMeshDescription*> Pointers = { &Description };
	if (!Mesh->BuildFromMeshDescriptions(Pointers, Params))
	{
		OutError = FString::Printf(TEXT("the engine could not build %s"), *PieceName);
		return nullptr;
	}
	Mesh->MarkPackageDirty();
	if (bPersist)
	{
		if (bNew)
		{
			FAssetRegistryModule::AssetCreated(Mesh);
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Mesh, *Filename, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved (check that your project's files are not read-only)"), *PackageName);
			return nullptr;
		}
	}
	return Mesh;
}
