// Copyright Joshua Eger. All Rights Reserved.
#include "MantleParametricOutfitAuthoring.h"

#include "MantleBodyTopologyData.h"
#include "MantleCosmeticExport.h"
#include "MantleGlbWriter.h"
#include "ChaosClothAsset/ClothAsset.h"
#include "ChaosClothAsset/CollectionClothFacade.h"
#include "ChaosOutfitAsset/CollectionOutfitFacade.h"
#include "ChaosOutfitAsset/Outfit.h"
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "ChaosOutfitAsset/SizedOutfitSource.h"
#include "Engine/SkeletalMesh.h"
#include "GeometryCollection/ManagedArrayCollection.h"
#include "SkinnedAssetCompiler.h"

namespace MantleOutfitAuthoringPrivate
{
	void Finish(USkinnedAsset* Asset)
	{
		USkinnedAsset* Assets[] = { Asset };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Assets);
	}

	bool IsId(const FString& Id)
	{
		if (Id.IsEmpty() || Id.Len() > 64) { return false; }
		for (const TCHAR C : Id)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-')) { return false; }
		}
		return true;
	}

	/** Mesh (skinned to Source's bones by joint name) as one LOD of render-only cloth whose skeletal mesh is Source. */
	bool WriteCollection(USkeletalMesh& Source, const FMantleGlbMesh& Mesh, TConstArrayView<UMaterialInterface*> Materials,
		TSharedRef<FManagedArrayCollection>& Collection, FString& Error)
	{
		using namespace UE::Chaos::ClothAsset;
		int32 Triangles = 0;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives) { Triangles += Primitive.Indices.Num() / 3; }
		if (!Mesh.IsSkinned() || Mesh.Positions.IsEmpty() || Triangles == 0 || Mesh.Primitives.IsEmpty())
		{ Error = TEXT("the authored garment must contain skinned render geometry"); return false; }
		TArray<int32> JointToBone;
		for (const FMantleGlbJoint& Joint : Mesh.Joints)
		{
			JointToBone.Add(Source.GetRefSkeleton().FindBoneIndex(Joint.Name));
			if (JointToBone.Last() == INDEX_NONE) { Error = TEXT("the garment's exported joint does not exist in its source skeleton"); return false; }
		}
		FCollectionClothFacade Cloth(Collection);
		Cloth.DefineSchema();
		Cloth.SetSkeletalMeshSoftObjectPathName(FSoftObjectPath(&Source));
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			if (Primitive.NumVertices <= 0 || Primitive.Indices.Num() % 3 != 0 || !Materials.IsValidIndex(Primitive.MaterialIndex))
			{ Error = TEXT("the authored garment has an invalid render section"); return false; }
			FCollectionClothRenderPatternFacade Pattern = Cloth.AddGetRenderPattern();
			Pattern.SetRenderMaterialSoftObjectPathName(FSoftObjectPath(Materials[Primitive.MaterialIndex]));
			Pattern.SetNumRenderVertices(Primitive.NumVertices);
			Pattern.SetNumRenderFaces(Primitive.Indices.Num() / 3);
			const int32 Offset = Pattern.GetRenderVerticesOffset();
			for (int32 Local = 0; Local < Primitive.NumVertices; ++Local)
			{
				const int32 Vertex = Primitive.FirstVertex + Local;
				if (!Mesh.Positions.IsValidIndex(Vertex) || !Mesh.Normals.IsValidIndex(Vertex) || !Mesh.UVs.IsValidIndex(Vertex))
				{ Error = TEXT("the authored garment section refers to missing vertex data"); return false; }
				Pattern.GetRenderPosition()[Local] = Mesh.Positions[Vertex];
				Pattern.GetRenderNormal()[Local] = Mesh.Normals[Vertex];
				const FVector3f Normal = Mesh.Normals[Vertex].GetSafeNormal();
				const FVector3f Axis = FMath::Abs(Normal.Z) < 0.9f ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
				const FVector4f Tangent = Mesh.Tangents.IsValidIndex(Vertex) ? Mesh.Tangents[Vertex]
					: FVector4f(FVector3f::CrossProduct(Axis, Normal).GetSafeNormal(), 1);
				const FVector3f TangentU(Tangent.X, Tangent.Y, Tangent.Z);
				Pattern.GetRenderTangentU()[Local] = TangentU;
				Pattern.GetRenderTangentV()[Local] = FVector3f::CrossProduct(Normal, TangentU) * Tangent.W;
				Pattern.GetRenderUVs()[Local].Add(Mesh.UVs[Vertex]);
				if (Mesh.UV1s.IsValidIndex(Vertex)) { Pattern.GetRenderUVs()[Local].Add(Mesh.UV1s[Vertex]); }
				Pattern.GetRenderColor()[Local] = Mesh.Colors.IsValidIndex(Vertex) ? FLinearColor(Mesh.Colors[Vertex]) : FLinearColor::White;
				float Sum = 0;
				for (int32 Influence = 0; Influence < Mesh.NumInfluences; ++Influence)
				{
					const int32 Index = Vertex * Mesh.NumInfluences + Influence;
					if (!Mesh.InfluenceWeights.IsValidIndex(Index) || !Mesh.InfluenceJoints.IsValidIndex(Index))
					{ Error = TEXT("the authored garment has incomplete skin weights"); return false; }
					const float Weight = Mesh.InfluenceWeights[Index];
					if (!FMath::IsFinite(Weight) || Weight < 0 || !JointToBone.IsValidIndex(Mesh.InfluenceJoints[Index]))
					{ Error = TEXT("the authored garment has invalid skin weights"); return false; }
					if (Weight == 0) { continue; }
					Pattern.GetRenderBoneIndices()[Local].Add(JointToBone[Mesh.InfluenceJoints[Index]]);
					Pattern.GetRenderBoneWeights()[Local].Add(Weight);
					Sum += Weight;
				}
				if (!FMath::IsNearlyEqual(Sum, 1.0f, 0.01f))
				{ Error = TEXT("the authored garment needs normalized, non-empty skin weights"); return false; }
			}
			for (int32 Face = 0; Face < Primitive.Indices.Num() / 3; ++Face)
			{
				FIntVector3 Indices;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 Index = Primitive.Indices[Face * 3 + Corner];
					if (Index >= static_cast<uint32>(Primitive.NumVertices))
					{ Error = TEXT("the authored garment has an invalid triangle index"); return false; }
					Indices[Corner] = Offset + static_cast<int32>(Index);
				}
				Pattern.GetRenderIndices()[Face] = Indices;
			}
		}
		if (!Cloth.HasValidRenderData()) { Error = TEXT("native cloth collection rejected the garment render data"); return false; }
		return true;
	}

	bool ReadCollection(USkeletalMesh& Source, int32 LOD, const TArray<int32>* KeepSlots, TSharedRef<FManagedArrayCollection>& Collection, FString& Error)
	{
		FMantleGlbMesh Mesh;
		TArray<UMaterialInterface*> Materials;
		int32 Triangles = 0;
		if (!FMantleCosmeticExport::ReadSkeletalMesh(&Source, LOD, Mesh, Materials, Triangles, Error)) { return false; }
		if (KeepSlots)
		{
			// glTF material i is the mesh's material slot UploadMaterialSlots()[i]; a dropped section's vertices stay in the
			// arrays unused, as each primitive reads only its own range.
			const TArray<int32> Slots = FMantleCosmeticExport::UploadMaterialSlots(&Source, LOD);
			Mesh.Primitives.RemoveAll([&Slots, KeepSlots](const FMantleGlbPrimitive& Primitive)
			{
				return !Slots.IsValidIndex(Primitive.MaterialIndex) || !KeepSlots->Contains(Slots[Primitive.MaterialIndex]);
			});
			if (Mesh.Primitives.IsEmpty()) { Error = TEXT("none of the garment's kept sections has geometry in this LOD"); return false; }
		}
		return WriteCollection(Source, Mesh, Materials, Collection, Error);
	}
}

UChaosOutfitAsset* FMantleParametricOutfitAuthoring::Create(TConstArrayView<FMantleOutfitAuthoredSource> Sources,
	UObject* Outer, FName Name, int32 InterpolationPoints, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& Error)
{
	using namespace MantleOutfitAuthoringPrivate;
	if (!IsInGameThread() || Sources.IsEmpty() || Sources.Num() > 8 || InterpolationPoints < 1 || InterpolationPoints > 5000)
	{ Error = TEXT("native Outfit authoring requires 1–8 source sizes and 1–5000 interpolation points on the editor thread"); return nullptr; }
	TSet<FString> Ids;
	for (const FMantleOutfitAuthoredSource& Row : Sources)
	{
		USkeletalMesh* Garment = Row.Garment.Get();
		USkeletalMesh* Body = Row.CombinedBodyHead.Get();
		const bool bGeometry = Row.Geometry.IsValid();
		if (!IsId(Row.Id) || Ids.Contains(Row.Id) || (!Garment && !bGeometry) || !Body || Garment == Body)
		{ Error = TEXT("each authored size needs a unique ID, its garment, and its matching combined body/head"); return nullptr; }
		Ids.Add(Row.Id);
		if (Garment) { Finish(Garment); }
		Finish(Body);
		if (!Body->GetMeshDescription(0))
		{ Error = TEXT("the combined authoring body/head must retain its native LOD0 MeshDescription for resizing"); return nullptr; }
	}
	UObject* Owner = Outer ? Outer : GetTransientPackage();
	const EObjectFlags Flags = Owner == GetTransientPackage() ? RF_Transient : (RF_Public | RF_Standalone | RF_Transactional);
	// The asking name when it is free (a new package's asset is named as its package), else a unique one.
	const FName AssetName = StaticFindObjectFast(nullptr, Owner, Name) ? MakeUniqueObjectName(Owner, UChaosOutfitAsset::StaticClass(), Name) : Name;
	UChaosOutfitAsset* Asset = NewObject<UChaosOutfitAsset>(Owner, AssetName, Flags);
	KeepAlive.Emplace(Asset);
	UChaosOutfit* Outfit = NewObject<UChaosOutfit>(Asset);
	KeepAlive.Emplace(Outfit);
	const FGuid GarmentGuid = FGuid::NewGuid();
	for (const FMantleOutfitAuthoredSource& Row : Sources)
	{
		USkeletalMesh* Garment = Row.Garment.Get();
		UChaosClothAsset* Cloth = NewObject<UChaosClothAsset>(Asset, FName(*(TEXT("CA_") + Row.Id)));
		KeepAlive.Emplace(Cloth); KeepAlive.Emplace(Row.CombinedBodyHead.Get());
		if (Garment) { KeepAlive.Emplace(Garment); }
		TArray<TSharedRef<const FManagedArrayCollection>> Collections;
		if (Row.Geometry.IsValid())
		{
			// Made in memory on the body's own bones: one LOD, the cloth's skeletal mesh the body.
			TSharedRef<FManagedArrayCollection> Collection = MakeShared<FManagedArrayCollection>();
			if (!WriteCollection(*Row.CombinedBodyHead.Get(), *Row.Geometry, Row.Materials, Collection, Error)) { return nullptr; }
			Collections.Add(Collection);
		}
		else
		{
			const int32 LODCount = UMantleBodyTopologyData::GetNumLODs(Garment);
			if (LODCount < 1) { Error = TEXT("the authored garment has no render LOD"); return nullptr; }
			for (int32 LOD = 0; LOD < LODCount; ++LOD)
			{
				TSharedRef<FManagedArrayCollection> Collection = MakeShared<FManagedArrayCollection>();
				if (!ReadCollection(*Garment, LOD, Row.KeepSlots.GetPtrOrNull(), Collection, Error)) { return nullptr; }
				Collections.Add(Collection);
			}
		}
		FText BuildError, BuildDetails;
		Cloth->Build(Collections, nullptr, &BuildError, &BuildDetails);
		Finish(Cloth);
		if (!BuildError.IsEmpty() || !Cloth->GetResourceForRendering())
		{ Error = FString::Printf(TEXT("native render-only cloth creation failed: %s %s"), *BuildError.ToString(), *BuildDetails.ToString()); return nullptr; }
		FChaosSizedOutfitSource NativeSource;
		NativeSource.SourceAsset = Cloth;
		NativeSource.SizeName = Row.Id;
		NativeSource.SourceBodyParts = { Row.CombinedBodyHead.Get() };
		NativeSource.NumResizingInterpolationPoints = InterpolationPoints;
		Outfit->Add(NativeSource, GarmentGuid);
	}
	const UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Facade(Outfit->GetOutfitCollection());
	if (!Facade.IsValid() || Facade.GetNumBodySizes() != Sources.Num() || !Facade.HasValidBodySize(true, false, true))
	{ Error = TEXT("native Outfit creation did not retain every source size and its resizing interpolation data"); return nullptr; }
	for (const FMantleOutfitAuthoredSource& Row : Sources)
	{
		const int32 Size = Facade.FindBodySize(Row.Id);
		if (Size == INDEX_NONE) { Error = TEXT("the native Outfit lost an authored source size"); return nullptr; }
		const auto Data = Facade.GetBodySizeInterpolationData(Size);
		if (Data.SampleIndices.Num() != 1 || Data.SampleRestPositions.Num() != 1
			|| Data.InterpolationWeights.Num() != 1 || Data.SampleIndices[0].IsEmpty()
			|| Data.SampleIndices[0].Num() != Data.SampleRestPositions[0].Num() || Data.InterpolationWeights[0].IsEmpty())
		{ Error = TEXT("each native Outfit source size must retain its own complete resizing interpolation data"); return nullptr; }
		if (Sources.Num() > 1 && Facade.GetBodySizeMeasurements(Size).IsEmpty())
		{ Error = TEXT("every multi-size native source body needs Epic body measurements for automatic source selection"); return nullptr; }
	}
	Asset->Build(Outfit);
	Finish(Asset);
	if (!Asset->GetOutfit() || !Asset->GetResourceForRendering())
	{ Error = TEXT("native Outfit creation produced no evaluated garment render data"); return nullptr; }
	return Asset;
}
