// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponRigidMesh.h"
#include "MantleAssetCleanup.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshReductionSettings.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

using UE::AnimationCore::FBoneWeight;

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponRigidMeshPrivate
{
	const FName RootBoneName(TEXT("root"));
	const FName MagazineBoneName(TEXT("magazine"));
	const FName MagazineSocketName(TEXT("Magazine"));

	/** One mesh's geometry as a mesh description, with the material each polygon group stands for. */
	struct FRigidGeo
	{
		FMeshDescription Description;
		/** Keyed by FPolygonGroupID::GetValue(). */
		TMap<int32, UMaterialInterface*> Materials;
		TMap<int32, FName> SlotNames;
	};

	struct FSlotInfo
	{
		UMaterialInterface* Material = nullptr;
		FName Slot;
		FName Imported;
	};

	/** Fills the geometry's per-group materials from the mesh's slot table: by imported slot name, else by slot name, else by group order. */
	void ResolveGroupMaterials(FRigidGeo& Geo, const TArray<FSlotInfo>& Slots)
	{
		const FStaticMeshConstAttributes Attributes(Geo.Description);
		const TPolygonGroupAttributesConstRef<FName> Names = Attributes.GetPolygonGroupMaterialSlotNames();
		int32 Order = 0;
		for (const FPolygonGroupID Group : Geo.Description.PolygonGroups().GetElementIDs())
		{
			const FName Name = Names.IsValid() ? Names.Get(Group) : NAME_None;
			int32 Found = INDEX_NONE;
			if (Name != NAME_None)
			{
				Found = Slots.IndexOfByPredicate([Name](const FSlotInfo& Slot) { return Slot.Imported == Name; });
				if (Found == INDEX_NONE)
				{
					Found = Slots.IndexOfByPredicate([Name](const FSlotInfo& Slot) { return Slot.Slot == Name; });
				}
			}
			if (Found == INDEX_NONE && Slots.IsValidIndex(Order))
			{
				Found = Order;
			}
			Geo.Materials.Add(Group.GetValue(), Found != INDEX_NONE ? Slots[Found].Material : nullptr);
			Geo.SlotNames.Add(Group.GetValue(), Found != INDEX_NONE && Slots[Found].Slot != NAME_None ? Slots[Found].Slot : (Name != NAME_None ? Name : FName(TEXT("Material"))));
			++Order;
		}
	}

	/** A static mesh's LOD0 read from its render data, for a mesh with no source description. */
	bool ReadStaticRenderData(const UStaticMesh* Mesh, FMeshDescription& Out)
	{
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const uint32 NumVertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		if (NumVertices == 0 || LOD.Sections.Num() == 0)
		{
			return false;
		}

		FStaticMeshAttributes Attributes(Out);
		Attributes.Register();
		const int32 NumUV = FMath::Max<int32>(1, LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
		Attributes.GetVertexInstanceUVs().SetNumChannels(NumUV);
		const bool bHasColors = LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == NumVertices;

		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(NumVertices);
		for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			const FVertexID VertexID = Out.CreateVertex();
			Attributes.GetVertexPositions().Set(VertexID, LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
			const FVertexInstanceID Instance = Out.CreateVertexInstance(VertexID);
			const FVector4f TangentZ = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex);
			const FVector4f TangentX = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Vertex);
			Attributes.GetVertexInstanceNormals().Set(Instance, FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z));
			Attributes.GetVertexInstanceTangents().Set(Instance, FVector3f(TangentX.X, TangentX.Y, TangentX.Z));
			Attributes.GetVertexInstanceBinormalSigns().Set(Instance, TangentZ.W < 0.0f ? -1.0f : 1.0f);
			for (int32 Channel = 0; Channel < NumUV; ++Channel)
			{
				Attributes.GetVertexInstanceUVs().Set(Instance, Channel, LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, Channel));
			}
			if (bHasColors)
			{
				const FLinearColor Color = LOD.VertexBuffers.ColorVertexBuffer.VertexColor(Vertex).ReinterpretAsLinear();
				Attributes.GetVertexInstanceColors().Set(Instance, FVector4f(Color.R, Color.G, Color.B, Color.A));
			}
			Instances.Add(Instance);
		}

		const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			const FPolygonGroupID Group = Out.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames().Set(Group, FName(*FString::Printf(TEXT("Section%d"), Section.MaterialIndex)));
			for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
			{
				const uint32 Base = Section.FirstIndex + Triangle * 3;
				if (static_cast<int32>(Base + 2) >= Indices.Num())
				{
					break;
				}
				const uint32 A = Indices[Base];
				const uint32 B = Indices[Base + 1];
				const uint32 C = Indices[Base + 2];
				if (A >= NumVertices || B >= NumVertices || C >= NumVertices)
				{
					continue;
				}
				const FVertexInstanceID Corners[3] = { Instances[A], Instances[B], Instances[C] };
				Out.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(Corners, 3));
			}
		}
		return Out.Triangles().Num() > 0;
	}

	bool ReadStatic(UStaticMesh* Mesh, FRigidGeo& Out, FString& OutError)
	{
		TArray<UStaticMesh*> Compiling = { Mesh };
		FStaticMeshCompilingManager::Get().FinishCompilation(Compiling);

		TArray<FSlotInfo> Slots;
		for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
		{
			FSlotInfo& Info = Slots.AddDefaulted_GetRef();
			Info.Material = Material.MaterialInterface;
			Info.Slot = Material.MaterialSlotName;
			Info.Imported = Material.ImportedMaterialSlotName;
		}

		bool bRead = false;
		if (Mesh->IsMeshDescriptionValid(0))
		{
			if (const FMeshDescription* Source = Mesh->GetMeshDescription(0))
			{
				Out.Description = *Source;
				bRead = Out.Description.Triangles().Num() > 0;
			}
		}
		if (bRead)
		{
			ResolveGroupMaterials(Out, Slots);
			return true;
		}

		Out.Description.Empty();
		if (!ReadStaticRenderData(Mesh, Out.Description))
		{
			OutError = FString::Printf(TEXT("%s has no geometry to read"), *Mesh->GetName());
			return false;
		}
		// The render data's sections name their material by index.
		int32 Order = 0;
		for (const FPolygonGroupID Group : Out.Description.PolygonGroups().GetElementIDs())
		{
			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			const int32 MaterialIndex = RenderData->LODResources[0].Sections.IsValidIndex(Order) ? RenderData->LODResources[0].Sections[Order].MaterialIndex : INDEX_NONE;
			Out.Materials.Add(Group.GetValue(), Slots.IsValidIndex(MaterialIndex) ? Slots[MaterialIndex].Material : nullptr);
			Out.SlotNames.Add(Group.GetValue(), Slots.IsValidIndex(MaterialIndex) && Slots[MaterialIndex].Slot != NAME_None ? Slots[MaterialIndex].Slot : FName(TEXT("Material")));
			++Order;
		}
		return true;
	}

	/** A skeletal mesh's LOD0 in its reference pose: a mesh description's positions are the bind pose already, so skinning changes nothing. */
	bool ReadSkeletal(USkeletalMesh* Mesh, FRigidGeo& Out, FString& OutError)
	{
		TArray<USkinnedAsset*> Compiling = { Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);

		bool bRead = false;
		if (Mesh->HasMeshDescription(0) && Mesh->CloneMeshDescription(0, Out.Description))
		{
			bRead = Out.Description.Triangles().Num() > 0;
		}
		if (!bRead)
		{
			Out.Description.Empty();
			const FSkeletalMeshModel* Model = Mesh->GetImportedModel();
			if (Model && Model->LODModels.IsValidIndex(0) && Model->LODModels[0].NumVertices > 0)
			{
				Model->LODModels[0].GetMeshDescription(Mesh, 0, Out.Description);
				bRead = Out.Description.Triangles().Num() > 0;
			}
		}
		if (!bRead)
		{
			OutError = FString::Printf(TEXT("%s has no geometry to read"), *Mesh->GetName());
			return false;
		}

		TArray<FSlotInfo> Slots;
		for (const FSkeletalMaterial& Material : Mesh->GetMaterials())
		{
			FSlotInfo& Info = Slots.AddDefaulted_GetRef();
			Info.Material = Material.MaterialInterface;
			Info.Slot = Material.MaterialSlotName;
			Info.Imported = Material.ImportedMaterialSlotName;
		}
		ResolveGroupMaterials(Out, Slots);
		return true;
	}

	bool ReadGeo(UObject* Mesh, FRigidGeo& Out, FString& OutError)
	{
		if (UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			return ReadStatic(Static, Out, OutError);
		}
		if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			return ReadSkeletal(Skeletal, Out, OutError);
		}
		OutError = FString::Printf(TEXT("%s is neither a static nor a skeletal mesh"), Mesh ? *Mesh->GetName() : TEXT("(none)"));
		return false;
	}

	/** The rigid mesh being assembled. */
	struct FRigidTarget
	{
		FMeshDescription Description;
		FSkeletalMeshAttributes Attributes;
		TArray<FSkeletalMaterial> Materials;
		TMap<UMaterialInterface*, FPolygonGroupID> GroupOfMaterial;
		TSet<FName> UsedSlotNames;
		FBox Bounds = FBox(ForceInit);
		int32 NumTriangles = 0;

		FRigidTarget()
			: Attributes(Description)
		{
		}
	};

	/** One source baked into the target: positioned by Placement, every vertex weighted whole to Bone. */
	void AppendGeo(const FRigidGeo& Source, const FTransform& Placement, const int32 Bone, FRigidTarget& Target)
	{
		const FStaticMeshConstAttributes From(Source.Description);
		const TVertexAttributesConstRef<FVector3f> FromPositions = From.GetVertexPositions();
		const TVertexInstanceAttributesConstRef<FVector3f> FromNormals = From.GetVertexInstanceNormals();
		const TVertexInstanceAttributesConstRef<FVector3f> FromTangents = From.GetVertexInstanceTangents();
		const TVertexInstanceAttributesConstRef<float> FromSigns = From.GetVertexInstanceBinormalSigns();
		const TVertexInstanceAttributesConstRef<FVector4f> FromColors = From.GetVertexInstanceColors();
		const TVertexInstanceAttributesConstRef<FVector2f> FromUVs = From.GetVertexInstanceUVs();

		FSkeletalMeshAttributes& To = Target.Attributes;
		const TVertexAttributesRef<FVector3f> ToPositions = To.GetVertexPositions();
		const TVertexInstanceAttributesRef<FVector3f> ToNormals = To.GetVertexInstanceNormals();
		const TVertexInstanceAttributesRef<FVector3f> ToTangents = To.GetVertexInstanceTangents();
		const TVertexInstanceAttributesRef<float> ToSigns = To.GetVertexInstanceBinormalSigns();
		const TVertexInstanceAttributesRef<FVector4f> ToColors = To.GetVertexInstanceColors();
		const TVertexInstanceAttributesRef<FVector2f> ToUVs = To.GetVertexInstanceUVs();
		FSkinWeightsVertexAttributesRef Weights = To.GetVertexSkinWeights();

		const FQuat Rotation = Placement.GetRotation();
		const FVector Scale = Placement.GetScale3D();
		const FVector InverseScale(
			FMath::IsNearlyZero(Scale.X) ? 1.0 : 1.0 / Scale.X,
			FMath::IsNearlyZero(Scale.Y) ? 1.0 : 1.0 / Scale.Y,
			FMath::IsNearlyZero(Scale.Z) ? 1.0 : 1.0 / Scale.Z);
		const bool bMirrored = (Scale.X * Scale.Y * Scale.Z) < 0.0;
		const float SignFlip = bMirrored ? -1.0f : 1.0f;

		// One polygon group per distinct material.
		TMap<int32, FPolygonGroupID> GroupMap;
		for (const FPolygonGroupID Group : Source.Description.PolygonGroups().GetElementIDs())
		{
			UMaterialInterface* Material = Source.Materials.FindRef(Group.GetValue());
			const FPolygonGroupID* Existing = Target.GroupOfMaterial.Find(Material);
			if (!Existing)
			{
				const FName Base = Source.SlotNames.FindRef(Group.GetValue());
				FName Slot = Base.IsNone() ? FName(TEXT("Material")) : Base;
				for (int32 Suffix = 1; Target.UsedSlotNames.Contains(Slot); ++Suffix)
				{
					Slot = FName(*FString::Printf(TEXT("%s_%d"), *Base.ToString(), Suffix));
				}
				Target.UsedSlotNames.Add(Slot);
				const FPolygonGroupID NewGroup = Target.Description.CreatePolygonGroup();
				To.GetPolygonGroupMaterialSlotNames().Set(NewGroup, Slot);
				Target.Materials.Add(FSkeletalMaterial(Material, Slot));
				Existing = &Target.GroupOfMaterial.Add(Material, NewGroup);
			}
			GroupMap.Add(Group.GetValue(), *Existing);
		}

		TArray<FBoneWeight> Influence;
		Influence.Add(FBoneWeight(static_cast<FBoneIndexType>(Bone), 1.0f));

		TMap<FVertexID, FVertexID> VertexMap;
		for (const FVertexID Vertex : Source.Description.Vertices().GetElementIDs())
		{
			const FVertexID NewVertex = Target.Description.CreateVertex();
			const FVector Position = Placement.TransformPosition(FVector(FromPositions.Get(Vertex)));
			ToPositions.Set(NewVertex, FVector3f(Position));
			Weights.Set(NewVertex, TArrayView<const FBoneWeight>(Influence));
			Target.Bounds += Position;
			VertexMap.Add(Vertex, NewVertex);
		}

		const int32 NumChannels = FMath::Min<int32>(FromUVs.IsValid() ? FromUVs.GetNumChannels() : 0, ToUVs.GetNumChannels());
		TMap<FVertexInstanceID, FVertexInstanceID> InstanceMap;
		for (const FVertexInstanceID Instance : Source.Description.VertexInstances().GetElementIDs())
		{
			const FVertexInstanceID NewInstance = Target.Description.CreateVertexInstance(VertexMap[Source.Description.GetVertexInstanceVertex(Instance)]);
			if (FromNormals.IsValid())
			{
				const FVector Normal = Rotation.RotateVector(FVector(FromNormals.Get(Instance)) * InverseScale).GetSafeNormal();
				ToNormals.Set(NewInstance, FVector3f(Normal));
			}
			if (FromTangents.IsValid())
			{
				const FVector Tangent = Placement.TransformVector(FVector(FromTangents.Get(Instance))).GetSafeNormal();
				ToTangents.Set(NewInstance, FVector3f(Tangent));
			}
			if (FromSigns.IsValid())
			{
				ToSigns.Set(NewInstance, FromSigns.Get(Instance) * SignFlip);
			}
			if (FromColors.IsValid())
			{
				ToColors.Set(NewInstance, FromColors.Get(Instance));
			}
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				ToUVs.Set(NewInstance, Channel, FromUVs.Get(Instance, Channel));
			}
			InstanceMap.Add(Instance, NewInstance);
		}

		for (const FTriangleID Triangle : Source.Description.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexInstanceID> Corners = Source.Description.GetTriangleVertexInstances(Triangle);
			FVertexInstanceID Mapped[3] = { InstanceMap[Corners[0]], InstanceMap[Corners[1]], InstanceMap[Corners[2]] };
			if (bMirrored)
			{
				Swap(Mapped[1], Mapped[2]);
			}
			Target.Description.CreateTriangle(GroupMap[Source.Description.GetTrianglePolygonGroup(Triangle).GetValue()], TArrayView<const FVertexInstanceID>(Mapped, 3));
			++Target.NumTriangles;
		}
	}

	int32 MaxUVChannels(const FRigidGeo& Geo)
	{
		const FStaticMeshConstAttributes Attributes(Geo.Description);
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		return UVs.IsValid() ? UVs.GetNumChannels() : 1;
	}

	FTransform WithoutScale(const FTransform& Transform)
	{
		return FTransform(Transform.GetRotation(), Transform.GetTranslation(), FVector::OneVector);
	}

	/** The asset already at PackageName/AssetName, loaded or on disk, or null. A loaded one is found in memory; one only on disk is loaded. */
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

	/**
	 * Clears the way for a re-export: the assets a previous export left at these names are moved aside, and their files removed, so the new assets are saved into a clean package. A first export finds nothing.
	 */
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
			// The old assets move to the transient package and become garbage; their files go below.
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

	/** A package for Name with no asset left in it under that name. */
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
			FMantleAssetCleanup::MoveAside({ Old }, /*bDeleteFiles*/ false);
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

	USkeletalMeshSocket* MakeSocket(UObject* Outer, const FName Name, const FName Bone, const FTransform& OnBone)
	{
		USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Outer, NAME_None, RF_Transactional);
		Socket->SocketName = Name;
		Socket->BoneName = Bone;
		Socket->RelativeLocation = OnBone.GetLocation();
		Socket->RelativeRotation = OnBone.Rotator();
		Socket->RelativeScale = OnBone.GetScale3D();
		return Socket;
	}
}

FMantleWeaponRigidMeshResult FMantleWeaponRigidMesh::Build(const FMantleWeaponRigidMeshInput& Input)
{
	using namespace MantleWeaponRigidMeshPrivate;
	FMantleWeaponRigidMeshResult Result;

	// Input checks.
	if (!Input.BodyMesh)
	{
		Result.Errors.Add(TEXT("there is no body mesh"));
		return Result;
	}
	if (Input.Name.IsEmpty() || !FName::IsValidXName(Input.Name, INVALID_OBJECTNAME_CHARACTERS) || Input.Name.Contains(TEXT("/")))
	{
		Result.Errors.Add(FString::Printf(TEXT("'%s' is not a valid asset name"), *Input.Name));
		return Result;
	}
	const FString MeshPackageName = Input.PackagePath / (TEXT("SKM_") + Input.Name);
	const FString SkeletonPackageName = Input.PackagePath / (TEXT("SK_") + Input.Name);
	if (!Input.PackagePath.StartsWith(TEXT("/")) || !FPackageName::IsValidLongPackageName(MeshPackageName) || !FPackageName::IsValidLongPackageName(SkeletonPackageName))
	{
		Result.Errors.Add(FString::Printf(TEXT("'%s' is not a valid package path"), *Input.PackagePath));
		return Result;
	}

	// Read every source.
	FRigidGeo Body;
	FString ReadError;
	if (!ReadGeo(Input.BodyMesh, Body, ReadError))
	{
		Result.Errors.Add(ReadError);
		return Result;
	}
	TArray<FRigidGeo> Parts;
	Parts.SetNum(Input.BodyParts.Num());
	for (int32 Index = 0; Index < Input.BodyParts.Num(); ++Index)
	{
		if (!ReadGeo(Input.BodyParts[Index].Mesh, Parts[Index], ReadError))
		{
			Result.Errors.Add(FString::Printf(TEXT("body part %d: %s"), Index, *ReadError));
			return Result;
		}
	}
	FRigidGeo MagazineGeo;
	if (Input.Magazine.IsSet() && !ReadGeo(Input.Magazine->Mesh, MagazineGeo, ReadError))
	{
		Result.Errors.Add(FString::Printf(TEXT("magazine: %s"), *ReadError));
		return Result;
	}
	const bool bMagazine = Input.Magazine.IsSet();

	// The geometry, weighted whole to its bone.
	FRigidTarget Target;
	Target.Attributes.Register();
	int32 NumUV = MaxUVChannels(Body);
	for (const FRigidGeo& Part : Parts)
	{
		NumUV = FMath::Max(NumUV, MaxUVChannels(Part));
	}
	if (bMagazine)
	{
		NumUV = FMath::Max(NumUV, MaxUVChannels(MagazineGeo));
	}
	Target.Attributes.GetVertexInstanceUVs().SetNumChannels(FMath::Clamp(NumUV, 1, 8));

	const FTransform& ToOutput = Input.BodyToOutput;
	const FTransform MagazineBoneInBody = bMagazine ? WithoutScale(Input.Magazine->InBody * ToOutput) : FTransform::Identity;
	{
		FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Target.Attributes.GetBoneNames();
		FSkeletalMeshAttributes::FBoneParentIndexAttributesRef BoneParents = Target.Attributes.GetBoneParentIndices();
		FSkeletalMeshAttributes::FBonePoseAttributesRef BonePoses = Target.Attributes.GetBonePoses();
		const FBoneID Root = Target.Attributes.CreateBone();
		BoneNames.Set(Root, RootBoneName);
		BoneParents.Set(Root, INDEX_NONE);
		BonePoses.Set(Root, FTransform::Identity);
		if (bMagazine)
		{
			const FBoneID Magazine = Target.Attributes.CreateBone();
			BoneNames.Set(Magazine, MagazineBoneName);
			BoneParents.Set(Magazine, 0);
			BonePoses.Set(Magazine, MagazineBoneInBody);
		}
	}

	AppendGeo(Body, ToOutput, 0, Target);
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		AppendGeo(Parts[Index], Input.BodyParts[Index].InBody * ToOutput, 0, Target);
	}
	if (bMagazine)
	{
		AppendGeo(MagazineGeo, Input.Magazine->InBody * ToOutput, 1, Target);
	}
	if (Target.NumTriangles == 0)
	{
		Result.Errors.Add(TEXT("the geometry is empty"));
		return Result;
	}
	const FBox Bounds = Target.Bounds;

	// The assets.
	DeleteExisting({ TPair<FString, FName>(MeshPackageName, FName(*(TEXT("SKM_") + Input.Name))), TPair<FString, FName>(SkeletonPackageName, FName(*(TEXT("SK_") + Input.Name))) });
	UPackage* MeshPackage = FreshPackage(MeshPackageName, FName(*(TEXT("SKM_") + Input.Name)));
	UPackage* SkeletonPackage = FreshPackage(SkeletonPackageName, FName(*(TEXT("SK_") + Input.Name)));
	USkeletalMesh* Mesh = MeshPackage ? NewObject<USkeletalMesh>(MeshPackage, FName(*(TEXT("SKM_") + Input.Name)), RF_Public | RF_Standalone) : nullptr;
	USkeleton* Skeleton = SkeletonPackage ? NewObject<USkeleton>(SkeletonPackage, FName(*(TEXT("SK_") + Input.Name)), RF_Public | RF_Standalone) : nullptr;
	if (!Mesh || !Skeleton)
	{
		Discard(Mesh);
		Discard(Skeleton);
		Result.Errors.Add(FString::Printf(TEXT("the assets could not be created under %s"), *Input.PackagePath));
		return Result;
	}

	Mesh->PreEditChange(nullptr);
	FReferenceSkeleton Reference;
	{
		FReferenceSkeletonModifier Modifier(Reference, nullptr);
		Modifier.Add(FMeshBoneInfo(RootBoneName, RootBoneName.ToString(), INDEX_NONE), FTransform::Identity);
		if (bMagazine)
		{
			Modifier.Add(FMeshBoneInfo(MagazineBoneName, MagazineBoneName.ToString(), 0), MagazineBoneInBody);
		}
	}
	Mesh->SetRefSkeleton(Reference);
	Mesh->GetRefBasesInvMatrix().Reset();
	Mesh->CalculateInvRefMatrices();
	Mesh->SetMaterials(Target.Materials);

	// LOD0 from the description; the normals and tangents are the sources', so the build keeps them.
	FSkeletalMeshLODInfo& LODInfo = Mesh->AddLODInfo();
	LODInfo.BuildSettings.bRecomputeNormals = false;
	LODInfo.BuildSettings.bRecomputeTangents = false;
	// The default reduction settings (half the triangles, base LOD 0) would reduce LOD0 itself in the build.
	LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	Mesh->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel);
	Mesh->CreateMeshDescription(0, MoveTemp(Target.Description));
	Mesh->CommitMeshDescription(0);
	Mesh->SetImportedBounds(FBoxSphereBounds(Bounds));

	// Sockets: the magazine's on its bone, the rest on root.
	for (const TPair<FName, FTransform>& Pair : Input.Sockets)
	{
		const FTransform SocketInOutput = WithoutScale(Pair.Value * ToOutput);
		if (bMagazine && Pair.Key == MagazineSocketName)
		{
			Mesh->AddSocket(MakeSocket(Mesh, Pair.Key, MagazineBoneName, SocketInOutput.GetRelativeTransform(MagazineBoneInBody)));
		}
		else
		{
			Mesh->AddSocket(MakeSocket(Mesh, Pair.Key, RootBoneName, SocketInOutput));
		}
	}

	if (!Skeleton->MergeAllBonesToBoneTree(Mesh, false))
	{
		Discard(Mesh);
		Discard(Skeleton);
		Result.Errors.Add(TEXT("the bones could not make a skeleton"));
		return Result;
	}
	Mesh->SetSkeleton(Skeleton);
	Mesh->SetPhysicsAsset(nullptr);

	Mesh->PostEditChange();
	TArray<USkinnedAsset*> Compiling = { Mesh };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	Skeleton->SetPreviewMesh(Mesh);

	FAssetRegistryModule::AssetCreated(Mesh);
	FAssetRegistryModule::AssetCreated(Skeleton);
	MeshPackage->MarkPackageDirty();
	SkeletonPackage->MarkPackageDirty();

	Result.Mesh = Mesh;
	Result.Skeleton = Skeleton;
	if (!SaveAsset(SkeletonPackage, Skeleton))
	{
		Result.Errors.Add(FString::Printf(TEXT("%s could not be saved"), *SkeletonPackageName));
	}
	if (!SaveAsset(MeshPackage, Mesh))
	{
		Result.Errors.Add(FString::Printf(TEXT("%s could not be saved"), *MeshPackageName));
	}
	return Result;
}

TMap<FName, FTransform> FMantleWeaponRigidMesh::ReadSocketsInMeshSpace(const USkeletalMesh* Mesh)
{
	TMap<FName, FTransform> Out;
	if (!Mesh)
	{
		return Out;
	}
	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	const TArray<FTransform>& Pose = Reference.GetRefBonePose();
	for (const USkeletalMeshSocket* Socket : Mesh->GetActiveSocketList())
	{
		if (!Socket)
		{
			continue;
		}
		FTransform BoneInMesh = FTransform::Identity;
		for (int32 Bone = Reference.FindBoneIndex(Socket->BoneName); Bone != INDEX_NONE; Bone = Reference.GetParentIndex(Bone))
		{
			BoneInMesh = BoneInMesh * Pose[Bone];
		}
		const FTransform Local(Socket->RelativeRotation, Socket->RelativeLocation, Socket->RelativeScale);
		Out.Add(Socket->SocketName, Local * BoneInMesh);
	}
	return Out;
}
