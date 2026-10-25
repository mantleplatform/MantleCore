// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisMesh.h"
#include "MantleAssetCleanup.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "HAL/FileManager.h"
#include "MantleSkinnedMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshUVChannelInfo.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshReductionSettings.h"
#include "SkeletalMeshTypes.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

using UE::AnimationCore::FBoneWeight;

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisMeshPrivate
{
	bool Refuse(FString& OutError, const FString& Why)
	{
		OutError = Why;
		return false;
	}

	/** The asset already at PackageName/AssetName, loaded or on disk, or null. */
	UObject* FindExisting(const FString& PackageName, const FName AssetName)
	{
		if (UPackage* Loaded = FindPackage(nullptr, *PackageName))
		{
			if (UObject* InMemory = FindObject<UObject>(Loaded, *AssetName.ToString()))
			{
				return InMemory;
			}
		}
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			return nullptr;
		}
		UPackage* FromDisk = LoadPackage(nullptr, *PackageName, LOAD_None);
		return FromDisk ? FindObject<UObject>(FromDisk, *AssetName.ToString()) : nullptr;
	}

	/** Clears the way for a remake: the assets a previous one left at these names are deleted, and their files removed, so the new ones are saved into clean packages. */
	void DeleteExisting(const TArray<TPair<FString, FName>>& Assets)
	{
		TArray<UObject*> Doomed;
		for (const TPair<FString, FName>& Asset : Assets)
		{
			if (UObject* Old = FindExisting(Asset.Key, Asset.Value))
			{
				if (USkinnedAsset* OldSkinned = Cast<USkinnedAsset>(Old))
				{
					TArray<USkinnedAsset*> Compiling = { OldSkinned };
					FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
				}
				Doomed.Add(Old);
			}
		}
		if (Doomed.Num() > 0)
		{
			FMantleAssetCleanup::MoveAside(Doomed, /*bDeleteFiles*/ false);
		}
		// A file the delete left behind (a package the registry did not know) would block the save.
		for (const TPair<FString, FName>& Asset : Assets)
		{
			if (UPackage* Stale = FindPackage(nullptr, *Asset.Key))
			{
				ResetLoaders(Stale);
			}
			const FString File = FPackageName::LongPackageNameToFilename(Asset.Key, FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*File))
			{
				IFileManager::Get().Delete(*File, false, true, true);
			}
		}
	}

	/** A package for AssetName with no asset left in it under that name. */
	UPackage* FreshPackage(const FString& PackageName, const FName AssetName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}
		if (UObject* Old = FindObject<UObject>(Package, *AssetName.ToString()))
		{
			// Still in memory after the delete (something held it): move it aside so the new asset can take the name.
			FAssetRegistryModule::AssetDeleted(Old);
			Old->ClearFlags(RF_Public | RF_Standalone);
			Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Old->MarkAsGarbage();
		}
		return Package;
	}

	bool SaveAsset(UPackage* Package, UObject* Asset)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *Filename, Args);
	}

	void Discard(UObject* Object)
	{
		if (Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		}
	}
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisMeshPrivate
{
	/**
	 * The geometry as a mesh description: one vertex and one vertex instance for each of the body's vertices (the shapes are flat-shaded, so none is
	 * shared), each vertex weighted as the body says, its triangles in the body's order (a description's order is the render order). OutSlotNames are the
	 * material slots' names: the one slot of a grey body, or the slots of the creator's model (ModelMaterials) that its sections name by MaterialIndex.
	 */
	void MakeDescription(const FMantleSkinnedMesh& Body, const TArray<FSkeletalMaterial>* ModelMaterials, const FMantleVehicleBodyExtras* Extras, FMeshDescription& OutDescription,
		TArray<FName>& OutSlotNames)
	{
		const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
		FSkeletalMeshAttributes Attributes(OutDescription);
		Attributes.Register();
		// The other UV channels the model brought (one entry per vertex each, up to the skeletal mesh's limit) and its vertex colours, when they fit the body.
		TArray<const TArray<FVector2f>*> ExtraUVs;
		const bool bColors = Extras && Extras->Colors.Num() == LOD.Positions.Num() && LOD.Positions.Num() > 0;
		if (Extras)
		{
			for (const TArray<FVector2f>& Channel : Extras->UVs)
			{
				if (Channel.Num() == LOD.Positions.Num() && 1 + ExtraUVs.Num() < MAX_TEXCOORDS)
				{
					ExtraUVs.Add(&Channel);
				}
			}
		}
		const bool bTangents = FMantleVehicleChassisMesh::HasUsableTangents(Body);
		Attributes.GetVertexInstanceUVs().SetNumChannels(1 + ExtraUVs.Num());
		{
			FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
			FSkeletalMeshAttributes::FBoneParentIndexAttributesRef BoneParents = Attributes.GetBoneParentIndices();
			FSkeletalMeshAttributes::FBonePoseAttributesRef BonePoses = Attributes.GetBonePoses();
			for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
			{
				const FBoneID Id = Attributes.CreateBone();
				BoneNames.Set(Id, Body.Bones[Bone].Name);
				BoneParents.Set(Id, Body.Bones[Bone].ParentIndex);
				BonePoses.Set(Id, Body.ReferencePose[Bone]);
			}
		}

		// The grey chassis has one slot, left empty; a creator's model has the slots it came with, and a polygon group for each of them its sections use.
		OutSlotNames.Reset();
		if (ModelMaterials)
		{
			for (const FSkeletalMaterial& Material : *ModelMaterials)
			{
				OutSlotNames.Add(Material.MaterialSlotName);
			}
		}
		if (OutSlotNames.Num() == 0)
		{
			FName OnlySlot = FName(TEXT("chassis"));
			if (LOD.Sections.Num() > 0 && !LOD.Sections[0].MaterialName.IsNone())
			{
				OnlySlot = LOD.Sections[0].MaterialName;
			}
			OutSlotNames.Add(OnlySlot);
		}
		TMap<int32, FPolygonGroupID> Groups;
		const auto GroupOf = [&](const int32 MaterialIndex)
		{
			const int32 Slot = OutSlotNames.IsValidIndex(MaterialIndex) ? MaterialIndex : 0;
			if (const FPolygonGroupID* Found = Groups.Find(Slot))
			{
				return *Found;
			}
			const FPolygonGroupID Made = OutDescription.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames().Set(Made, OutSlotNames[Slot]);
			Groups.Add(Slot, Made);
			return Made;
		};
		// A mesh with no sections is one group of all its triangles.
		GroupOf(0);

		const TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		const TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		const TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		const TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		const TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
		const int32 NumVertices = LOD.Positions.Num();
		const int32 Slots = LOD.NumInfluences;
		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(NumVertices);
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			const FVertexID NewVertex = OutDescription.CreateVertex();
			Positions.Set(NewVertex, LOD.Positions[Vertex]);
			TArray<FBoneWeight> Influence;
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				const uint8 Weight = LOD.InfluenceWeights[Vertex * Slots + Slot];
				if (Weight > 0)
				{
					Influence.Add(FBoneWeight(static_cast<FBoneIndexType>(LOD.InfluenceBones[Vertex * Slots + Slot]), static_cast<float>(Weight) / 255.0f));
				}
			}
			Weights.Set(NewVertex, TArrayView<const FBoneWeight>(Influence));
			const FVertexInstanceID Instance = OutDescription.CreateVertexInstance(NewVertex);
			Normals.Set(Instance, LOD.Normals[Vertex]);
			if (bTangents)
			{
				// The creator's own tangent frame: the sign is the binormal's, as the packed tangent basis of a mesh holds it.
				const FVector4f& Tangent = LOD.Tangents[Vertex];
				Tangents.Set(Instance, FVector3f(Tangent.X, Tangent.Y, Tangent.Z));
				Signs.Set(Instance, Tangent.W < 0.0f ? -1.0f : 1.0f);
			}
			else
			{
				Signs.Set(Instance, 1.0f);
			}
			UVs.Set(Instance, 0, LOD.UVs[Vertex]);
			for (int32 Channel = 0; Channel < ExtraUVs.Num(); ++Channel)
			{
				UVs.Set(Instance, 1 + Channel, (*ExtraUVs[Channel])[Vertex]);
			}
			if (bColors)
			{
				// A skeletal mesh's colour attribute is the byte colour over 255 (the build writes it back with no gamma), so an FColor round trips exactly.
				Colors.Set(Instance, FVector4f(Extras->Colors[Vertex].ReinterpretAsLinear()));
			}
			Instances.Add(Instance);
		}
		const auto AddTriangles = [&](const int32 Begin, const int32 End, const FPolygonGroupID Group)
		{
			for (int32 First = Begin; First + 2 < End && First + 2 < LOD.Indices.Num(); First += 3)
			{
				const FVertexInstanceID Corners[3] = { Instances[LOD.Indices[First]], Instances[LOD.Indices[First + 1]], Instances[LOD.Indices[First + 2]] };
				OutDescription.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(Corners, 3));
			}
		};
		if (LOD.Sections.Num() == 0)
		{
			AddTriangles(0, LOD.Indices.Num(), GroupOf(0));
		}
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			AddTriangles(Section.FirstIndex, Section.FirstIndex + Section.IndexCount, GroupOf(Section.MaterialIndex));
		}
	}

	/**
	 * LOD0 is the body as made, with the shapes' own flat normals and tangents from the UVs. A new LOD's default reduction settings keep half the
	 * triangles and the build then reduces LOD0 on its own, so they are set to keep all of them.
	 */
	void SetLodZeroSettings(FSkeletalMeshLODInfo& LODInfo, const bool bRecomputeTangents)
	{
		LODInfo.BuildSettings.bRecomputeNormals = false;
		// A body with tangents of its own (a creator's model) keeps them; the grey chassis has none and takes them from its UVs.
		LODInfo.BuildSettings.bRecomputeTangents = bRecomputeTangents;
		LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
		LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	}

	/** True when Mesh and Skeleton are a chassis body this Body can be written into without remaking them: the same bones in the same order, one LOD. */
	bool CanRebuildInPlace(const USkeletalMesh& Mesh, const USkeleton& Skeleton, const FMantleSkinnedMesh& Body)
	{
		const FSkeletalMeshModel* Model = Mesh.GetImportedModel();
		if (Mesh.GetSkeleton() != &Skeleton || Mesh.GetLODNum() != 1 || !Model || Model->LODModels.Num() != 1)
		{
			return false;
		}
		const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
		if (Reference.GetRawBoneNum() != Body.Bones.Num())
		{
			return false;
		}
		const TArray<FMeshBoneInfo>& Info = Reference.GetRawRefBoneInfo();
		for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
		{
			if (Info[Bone].Name != Body.Bones[Bone].Name || Info[Bone].ParentIndex != Body.Bones[Bone].ParentIndex)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Writes the new body into the mesh and skeleton that are already there, in place (the pattern the avatar fixers use to rewrite a mesh): the same
	 * objects, so a vehicle asset, a spawn in the level and an open viewport that name or show them keep them, nothing is deleted, and a save that
	 * fails leaves the vehicle with the body it had. The reference pose moves to the new wheel places, the description is replaced, and leaving the
	 * scope rebuilds the mesh and puts its render state back.
	 */
	void RebuildInPlace(USkeletalMesh& Mesh, FMeshDescription&& Description, const FMantleSkinnedMesh& Body, const FBox& Bounds, const TArray<FSkeletalMaterial>* ModelMaterials,
		const bool bRecomputeTangents)
	{
		TArray<USkinnedAsset*> Compiling = { &Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		{
			FScopedSkeletalMeshPostEditChange PostEdit(&Mesh);
			{
				FReferenceSkeletonModifier Modifier(Mesh.GetRefSkeleton(), Mesh.GetSkeleton());
				for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
				{
					Modifier.UpdateRefPoseTransform(Bone, Body.ReferencePose[Bone]);
				}
			}
			Mesh.GetRefBasesInvMatrix().Reset();
			Mesh.CalculateInvRefMatrices();
			if (FSkeletalMeshLODInfo* LODInfo = Mesh.GetLODInfo(0))
			{
				SetLodZeroSettings(*LODInfo, bRecomputeTangents);
			}
			if (ModelMaterials && ModelMaterials->Num() > 0)
			{
				// A creator's model brings its own slots; the sections of the new description name them.
				Mesh.SetMaterials(*ModelMaterials);
			}
			Mesh.CreateMeshDescription(0, MoveTemp(Description));
			USkeletalMesh::FCommitMeshDescriptionParams Commit;
			Commit.bForceUpdate = true;
			Mesh.CommitMeshDescription(0, Commit);
			Mesh.SetImportedBounds(FBoxSphereBounds(Bounds));
		} // PostEditChange: the build.
	}

	/** Makes a mesh and a skeleton that are new (or take the place of ones that could not be reused) in their packages. False, with the reason, when they cannot be. */
	bool CreateFresh(const FMantleSkinnedMesh& Body, FMeshDescription&& Description, const TArray<FName>& SlotNames, const TArray<FSkeletalMaterial>* ModelMaterials, const FBox& Bounds,
		const bool bRecomputeTangents, const FString& MeshPackageName, const FString& SkeletonPackageName, const FString& MeshName, const FString& SkeletonName, const bool bPersist, USkeletalMesh*& OutMesh,
		USkeleton*& OutSkeleton, FString& OutError)
	{
		if (bPersist)
		{
			DeleteExisting({ TPair<FString, FName>(MeshPackageName, FName(*MeshName)), TPair<FString, FName>(SkeletonPackageName, FName(*SkeletonName)) });
		}
		UPackage* MeshPackage = FreshPackage(MeshPackageName, FName(*MeshName));
		UPackage* SkeletonPackage = FreshPackage(SkeletonPackageName, FName(*SkeletonName));
		USkeletalMesh* Mesh = MeshPackage ? NewObject<USkeletalMesh>(MeshPackage, FName(*MeshName), RF_Public | RF_Standalone) : nullptr;
		USkeleton* Skeleton = SkeletonPackage ? NewObject<USkeleton>(SkeletonPackage, FName(*SkeletonName), RF_Public | RF_Standalone) : nullptr;
		if (!Mesh || !Skeleton)
		{
			Discard(Mesh);
			Discard(Skeleton);
			return Refuse(OutError, TEXT("it could not be created in that folder"));
		}

		Mesh->PreEditChange(nullptr);
		FReferenceSkeleton Reference;
		{
			FReferenceSkeletonModifier Modifier(Reference, nullptr);
			for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
			{
				Modifier.Add(FMeshBoneInfo(Body.Bones[Bone].Name, Body.Bones[Bone].Name.ToString(), Body.Bones[Bone].ParentIndex), Body.ReferencePose[Bone]);
			}
		}
		Mesh->SetRefSkeleton(Reference);
		Mesh->GetRefBasesInvMatrix().Reset();
		Mesh->CalculateInvRefMatrices();
		TArray<FSkeletalMaterial> Materials;
		if (ModelMaterials && ModelMaterials->Num() > 0)
		{
			Materials = *ModelMaterials;
		}
		else
		{
			// No material of its own: the body draws Mantle's default surface.
			Materials.Add(FSkeletalMaterial(static_cast<UMaterialInterface*>(nullptr), SlotNames[0]));
		}
		Mesh->SetMaterials(Materials);

		SetLodZeroSettings(Mesh->AddLODInfo(), bRecomputeTangents);
		Mesh->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel);
		Mesh->CreateMeshDescription(0, MoveTemp(Description));
		Mesh->CommitMeshDescription(0);
		Mesh->SetImportedBounds(FBoxSphereBounds(Bounds));

		if (!Skeleton->MergeAllBonesToBoneTree(Mesh, false))
		{
			Discard(Mesh);
			Discard(Skeleton);
			return Refuse(OutError, TEXT("its bones could not make a skeleton"));
		}
		Mesh->SetSkeleton(Skeleton);
		Mesh->SetPhysicsAsset(nullptr);

		Mesh->PostEditChange();
		TArray<USkinnedAsset*> Compiling = { Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		Skeleton->SetPreviewMesh(Mesh);

		if (bPersist)
		{
			FAssetRegistryModule::AssetCreated(Mesh);
			FAssetRegistryModule::AssetCreated(Skeleton);
		}
		OutMesh = Mesh;
		OutSkeleton = Skeleton;
		return true;
	}
}

bool FMantleVehicleChassisMesh::HasUsableTangents(const FMantleSkinnedMesh& Body)
{
	if (Body.LODs.Num() == 0)
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	if (LOD.Positions.Num() == 0 || LOD.Tangents.Num() != LOD.Positions.Num())
	{
		return false;
	}
	for (const FVector4f& Tangent : LOD.Tangents)
	{
		if (Tangent.ContainsNaN() || FVector3f(Tangent.X, Tangent.Y, Tangent.Z).SizeSquared() < 0.25f)
		{
			return false;
		}
	}
	return true;
}

USkeletalMesh* FMantleVehicleChassisMesh::Create(const FMantleSkinnedMesh& Body, const FString& PackagePath, const FString& Name, FString& OutError,
	const TArray<FSkeletalMaterial>* ModelMaterials, const FMantleVehicleBodyExtras* Extras)
{
	using namespace MantleVehicleChassisMeshPrivate;
	if (Body.LODs.Num() == 0 || Body.LODs[0].Positions.Num() == 0 || Body.Bones.Num() == 0 || Body.ReferencePose.Num() != Body.Bones.Num())
	{
		Refuse(OutError, TEXT("the chassis has no body to make a mesh of"));
		return nullptr;
	}
	if (Name.IsEmpty() || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS) || Name.Contains(TEXT("/")))
	{
		Refuse(OutError, FString::Printf(TEXT("'%s' is not a name a vehicle can be saved under"), *Name));
		return nullptr;
	}
	const FString MeshName = MeshAssetName(Name);
	const FString SkeletonName = SkeletonAssetName(Name);
	const FString MeshPackageName = PackagePath / MeshName;
	const FString SkeletonPackageName = PackagePath / SkeletonName;
	// A throwaway package (a test's, under /Temp) is under a read-only root, which a real folder never is.
	const bool bPersist = !IsScratchPath(PackagePath);
	if (!PackagePath.StartsWith(TEXT("/")) || !FPackageName::IsValidLongPackageName(MeshPackageName, !bPersist) || !FPackageName::IsValidLongPackageName(SkeletonPackageName, !bPersist))
	{
		Refuse(OutError, FString::Printf(TEXT("'%s' is not a folder a vehicle can be saved in"), *PackagePath));
		return nullptr;
	}

	FMeshDescription Description;
	TArray<FName> SlotNames;
	MakeDescription(Body, ModelMaterials, Extras, Description, SlotNames);
	const bool bRecomputeTangents = !HasUsableTangents(Body);
	const FBox3f Box = Body.ComputeBounds(0);
	const FBox Bounds(FVector(Box.Min), FVector(Box.Max));

	// The assets: ones already there are written into in place; only when they are not a body this one fits are they deleted and made again.
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(FindExisting(MeshPackageName, FName(*MeshName)));
	USkeleton* Skeleton = Cast<USkeleton>(FindExisting(SkeletonPackageName, FName(*SkeletonName)));
	if (Mesh && Skeleton && CanRebuildInPlace(*Mesh, *Skeleton, Body))
	{
		RebuildInPlace(*Mesh, MoveTemp(Description), Body, Bounds, ModelMaterials, bRecomputeTangents);
	}
	else if (!CreateFresh(Body, MoveTemp(Description), SlotNames, ModelMaterials, Bounds, bRecomputeTangents, MeshPackageName, SkeletonPackageName, MeshName, SkeletonName, bPersist, Mesh, Skeleton, OutError))
	{
		return nullptr;
	}

	Mesh->MarkPackageDirty();
	Skeleton->MarkPackageDirty();
	if (bPersist)
	{
		if (!SaveAsset(Skeleton->GetOutermost(), Skeleton))
		{
			Refuse(OutError, FString::Printf(TEXT("%s could not be saved (check that your project's files are not read-only)"), *SkeletonPackageName));
			return nullptr;
		}
		if (!SaveAsset(Mesh->GetOutermost(), Mesh))
		{
			Refuse(OutError, FString::Printf(TEXT("%s could not be saved (check that your project's files are not read-only)"), *MeshPackageName));
			return nullptr;
		}
	}
	return Mesh;
}
