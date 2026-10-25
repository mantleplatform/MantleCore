// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticExport.h"
#include "MantleAvatarRefitFixer.h"
#include "MantleAnimClipWriter.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleBodyOffsetsBuilder.h"
#include "MantleBodyCoverage.h"
#include "MantleBodyCorrespondence.h"
#include "MantleBodyShape.h"
#include "MantleFaceLayout.h"
#include "MantleFitBodies.h"
#include "MantleBodyTopologyData.h"
#include "MantleCatalogGarmentFit.h"
#include "MantleClothingColorMeasure.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEmoteCheck.h"
#include "MantleModelThumbnail.h"
#include "MantleOverlayWorn.h"
#include "MantleReferenceBodies.h"
#include "MantleEmoteRetarget.h"
#include "MantleGarmentFitter.h"
#include "MantleGarmentOutfit.h"
#include "MantleHairCards.h"
#include "MantleHairColor.h"
#include "MantleHairCardsFit.h"
#include "MantleNativeHairProfile.h"
#include "GroomAsset.h"
#include "MantleHairSourceDiscovery.h"
#include "MantleHeadFit.h"
#include "MantleMeshReduce.h"
#include "MantleAccessorySettings.h"
#include "MantleNeckSamples.h"
#include "MantleNativeEyeExport.h"
#include "MantleOverlayChecks.h"
#include "MantleOutfitSections.h"
#include "MantleParametricOutfit.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceMaterial.h"
#include "MantlePng.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"
#include "MantleStaticMeshReader.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Exporters/GLTFExporter.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "LODUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Options/GLTFExportOptions.h"
#include "MeshDescription.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "RHIGlobals.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshReductionSettings.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshResources.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticExportPrivate
{
	/** Every MetaHuman face exported at Medium has these triangles at its reference face LOD. */
	constexpr int32 ReferenceFaceTriangles = 6872;

	/** The package note for a body export whose exact shading twins were merged back into the canonical layout. */
	FString MergedBodySplitsNote(const TArray<FString>& Splits)
	{
		return FString::Printf(TEXT("Body export LOD0: merged %d exact shading twins [%s]; retained each lower physical ID's normal/tangent; canonical UV order and full triangles verified."),
			Splits.Num(), *FString::Join(Splits, TEXT(", ")));
	}

	/** The most influences a vertex keeps in an upload GLB (the worker's cook.MaxSkinInfluences, MSK1's limit). */
	constexpr int32 MaxSkinInfluences = 8;

	/** The shared face appearance maps were authored for this indexed MetaHuman UV0 profile. */
	constexpr int32 ClientFaceSkinUVSetCount = 2652;
	constexpr uint32 ClientFaceSkinUVSetHash = 0xe581f5fbu;
	constexpr int32 ClientEyeUVSetCount = 122;
	constexpr uint32 ClientEyeUVSetHash = 0xad78fd54u;
	constexpr int32 ClientFaceSkinUVTriangleCount = 5048;
	constexpr uint32 ClientFaceSkinUVTriangleHash = 0xfd434c83u;
	constexpr int32 ClientEyeUVTriangleCount = 216;
	constexpr uint32 ClientEyeUVTriangleHash = 0x668f3d32u;

	struct FQuantizedUVTriangle
	{
		FIntPoint Pairs[3];
	};

	bool LessQuantizedUVPair(const FIntPoint& Left, const FIntPoint& Right)
	{
		return Left.X == Right.X ? Left.Y < Right.Y : Left.X < Right.X;
	}

	bool LessQuantizedUVTriangle(const FQuantizedUVTriangle& Left, const FQuantizedUVTriangle& Right)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			if (Left.Pairs[Corner] == Right.Pairs[Corner]) continue;
			return LessQuantizedUVPair(Left.Pairs[Corner], Right.Pairs[Corner]);
		}
		return false;
	}

	bool AppendQuantizedUVTriangles(const FMantleGlbMesh& Mesh, const FMantleGlbPrimitive& Primitive,
		TArray<FQuantizedUVTriangle>& OutTriangles, FString& OutError)
	{
		if (Primitive.Indices.Num() % 3 != 0 || Primitive.FirstVertex < 0 || Primitive.NumVertices <= 0
			|| Primitive.FirstVertex + Primitive.NumVertices > Mesh.UVs.Num())
		{
			OutError = TEXT("The face primitive has an invalid UV0 range or incomplete triangle indices.");
			return false;
		}
		for (int32 Index = 0; Index < Primitive.Indices.Num(); Index += 3)
		{
			FQuantizedUVTriangle Triangle;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 LocalVertex = Primitive.Indices[Index + Corner];
				if (LocalVertex >= static_cast<uint32>(Primitive.NumVertices))
				{
					OutError = TEXT("The face primitive references an out-of-range UV0 vertex.");
					return false;
				}
				const FVector2f& UV = Mesh.UVs[Primitive.FirstVertex + static_cast<int32>(LocalVertex)];
				if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y)
					|| UV.X * 2048.f < MIN_int32 || UV.X * 2048.f > MAX_int32
					|| UV.Y * 2048.f < MIN_int32 || UV.Y * 2048.f > MAX_int32)
				{
					OutError = TEXT("The face primitive has a non-finite or out-of-range UV0 coordinate.");
					return false;
				}
				Triangle.Pairs[Corner] = FIntPoint(FMath::RoundToInt(UV.X * 2048.f), FMath::RoundToInt(UV.Y * 2048.f));
			}
			if (LessQuantizedUVPair(Triangle.Pairs[1], Triangle.Pairs[0])) Swap(Triangle.Pairs[0], Triangle.Pairs[1]);
			if (LessQuantizedUVPair(Triangle.Pairs[2], Triangle.Pairs[1])) Swap(Triangle.Pairs[1], Triangle.Pairs[2]);
			if (LessQuantizedUVPair(Triangle.Pairs[1], Triangle.Pairs[0])) Swap(Triangle.Pairs[0], Triangle.Pairs[1]);
			OutTriangles.Add(Triangle);
		}
		return true;
	}

	uint32 HashQuantizedUVTriangleMultiset(TArray<FQuantizedUVTriangle>& Triangles)
	{
		Triangles.Sort([](const FQuantizedUVTriangle& Left, const FQuantizedUVTriangle& Right)
		{
			return LessQuantizedUVTriangle(Left, Right);
		});
		TArray<uint8> Bytes;
		Bytes.Reserve(Triangles.Num() * 3 * 8);
		for (const FQuantizedUVTriangle& Triangle : Triangles)
		{
			for (const FIntPoint& Pair : Triangle.Pairs)
			{
				const uint32 Values[] = { static_cast<uint32>(Pair.X), static_cast<uint32>(Pair.Y) };
				for (const uint32 Value : Values)
				{
					Bytes.Add(static_cast<uint8>(Value & 0xff));
					Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
					Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
					Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
				}
			}
		}
		return FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
	}

	bool HasClientAppearanceUVTriangleProfile(const FMantleGlbMesh& Mesh, const int32 MaterialIndex,
		const int32 ExpectedCount, const uint32 ExpectedHash, const TCHAR* SectionRole, FString& OutError)
	{
		TArray<FQuantizedUVTriangle> Triangles;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			if (Primitive.MaterialIndex == MaterialIndex
				&& !AppendQuantizedUVTriangles(Mesh, Primitive, Triangles, OutError))
			{
				OutError = FString::Printf(TEXT("The converted face %s section has invalid triangle UV0 evidence: %s"), SectionRole, *OutError);
				return false;
			}
		}
		const uint32 ActualHash = HashQuantizedUVTriangleMultiset(Triangles);
		if (Triangles.Num() != ExpectedCount || ActualHash != ExpectedHash)
		{
			OutError = FString::Printf(TEXT("The converted face %s section does not match the supported UV0 triangle profile (got %d / %08x, expected %d / %08x)."),
				SectionRole, Triangles.Num(), ActualHash, ExpectedCount, ExpectedHash);
			return false;
		}
		return true;
	}

	/** A mesh LOD read for export, before its skin is fitted to a contract skeleton. */
	struct FMeshSource
	{
		FMantleGlbMesh Glb;
		const USkeletalMesh* NativeSource = nullptr;
		TArray<FString> MergedBodySplits;

		/** Per glTF material: the material to take textures from. */
		TArray<UMaterialInterface*> Materials;

		/** Static meshes: per glTF material, the mesh's material slot it came from (used to write materials[]). */
		TArray<int32> SlotIndices;

		/** Skeletal only: per mesh bone, its component-space reference pose and whether any vertex weights it. */
		TArray<FTransform> ComponentPose;
		TArray<bool> Weighted;

		/** The most non-zero influences any vertex has (after the cap to MaxSkinInfluences). */
		int32 MostInfluences = 0;

		/** Vertices that had more than MaxSkinInfluences influences, and the largest weight dropped from one. */
		int32 CappedVertices = 0;
		float LargestDroppedWeight = 0.0f;

		/** The weighted bones off the chosen skeleton that its fit folded into their nearest kept ancestors. */
		TArray<FMantleFoldedBone> FoldedBones;

		FBox3f Bounds = FBox3f(ForceInit);
		int32 LODIndex = 0;
		int32 NumTriangles = 0;
	};

	FString SlotName(const FName& Name, const UMaterialInterface* Material, int32 SlotIndex)
	{
		if (!Name.IsNone())
		{
			return Name.ToString();
		}
		return Material ? Material->GetName() : FString::Printf(TEXT("Material%d"), SlotIndex);
	}

	int32 SectionSlot(const USkeletalMesh* Mesh, int32 LODIndex, int32 SectionIndex, int32 RenderMaterialIndex)
	{
		const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIndex);
		const int32 NumSlots = Mesh->GetMaterials().Num();
		if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(SectionIndex))
		{
			const int32 Mapped = LODInfo->LODMaterialMap[SectionIndex];
			if (Mapped >= 0 && Mapped < NumSlots)
			{
				return Mapped;
			}
		}
		return FMath::Clamp(RenderMaterialIndex, 0, FMath::Max(0, NumSlots - 1));
	}

	bool IsEyeParallelBone(const FString& BoneName, const bool bLeft)
	{
		return BoneName.Equals(bLeft ? TEXT("FACIAL_L_EyeParallel") : TEXT("FACIAL_R_EyeParallel"), ESearchCase::IgnoreCase);
	}

	uint32 ComputeQuantizedUVSetHash(TConstArrayView<FVector2f> UVs, int32& OutUniquePairCount)
	{
		TArray<FIntPoint> Quantized;
		Quantized.Reserve(UVs.Num());
		for (const FVector2f& UV : UVs)
		{
			Quantized.Add(FIntPoint(FMath::RoundToInt(UV.X * 2048.f), FMath::RoundToInt(UV.Y * 2048.f)));
		}
		Quantized.Sort(LessQuantizedUVPair);
		uint32 Hash = 0;
		OutUniquePairCount = 0;
		FIntPoint Previous(MAX_int32, MAX_int32);
		for (const FIntPoint& UV : Quantized)
		{
			if (OutUniquePairCount > 0 && UV == Previous) continue;
			Hash = FCrc::MemCrc32(&UV.X, sizeof(UV.X), Hash);
			Hash = FCrc::MemCrc32(&UV.Y, sizeof(UV.Y), Hash);
			Previous = UV;
			++OutUniquePairCount;
		}
		return Hash;
	}

	bool HasClientAppearanceUVSet(const FMantleGlbMesh& Mesh, const int32 MaterialIndex, const int32 ExpectedCount,
		const uint32 ExpectedHash, const TCHAR* SectionRole, FString& OutError)
	{
		TSet<int32> IndexedVertices;
		bool bFoundPrimitive = false;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			if (Primitive.MaterialIndex != MaterialIndex) continue;
			bFoundPrimitive = true;
			if (Primitive.FirstVertex < 0 || Primitive.NumVertices <= 0 || Primitive.FirstVertex + Primitive.NumVertices > Mesh.UVs.Num())
			{
				OutError = FString::Printf(TEXT("The converted face %s section has an invalid UV0 vertex range."), SectionRole);
				return false;
			}
			for (const uint32 LocalVertex : Primitive.Indices)
			{
				if (LocalVertex >= static_cast<uint32>(Primitive.NumVertices))
				{
					OutError = FString::Printf(TEXT("The converted face %s section has an invalid indexed UV0 vertex."), SectionRole);
					return false;
				}
				IndexedVertices.Add(Primitive.FirstVertex + static_cast<int32>(LocalVertex));
			}
		}
		if (!bFoundPrimitive || IndexedVertices.IsEmpty())
		{
			OutError = FString::Printf(TEXT("The converted face %s section has no indexed UV0 vertices."), SectionRole);
			return false;
		}
		TArray<int32> OrderedVertices = IndexedVertices.Array();
		OrderedVertices.Sort();
		TArray<FVector2f> UVs;
		UVs.Reserve(OrderedVertices.Num());
		for (const int32 Vertex : OrderedVertices)
		{
			const FVector2f UV = Mesh.UVs[Vertex];
			if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y))
			{
				OutError = FString::Printf(TEXT("The converted face %s section has non-finite UV0 coordinates."), SectionRole);
				return false;
			}
			UVs.Add(UV);
		}
		int32 ActualCount = 0;
		const uint32 ActualHash = ComputeQuantizedUVSetHash(UVs, ActualCount);
		if (ActualCount != ExpectedCount || ActualHash != ExpectedHash)
		{
			OutError = FString::Printf(TEXT("The converted face %s section does not match the supported client UV0 profile (got %d / %08x, expected %d / %08x)."),
				SectionRole, ActualCount, ActualHash, ExpectedCount, ExpectedHash);
			return false;
		}
		return true;
	}

	/**
	 * One LOD of a skeletal mesh from its render data (CPU copies, which the editor keeps): render
	 * vertices and triangles in their own order, one primitive per render section, the full raw
	 * reference skeleton as joints, weights as floats. KeepSlots, when given, reads only the sections drawn with those material
	 * slots, their vertices packed one section after another (a garment needs no source vertex order).
	 * A whole LOD whose reference topology is in its mesh description's order (HasReferenceTopology) is read in
	 * that order instead, through each reference vertex's render vertex, so its upload has the reference topology.
	 */
	bool ReadSkeletalLOD(const USkeletalMesh* Mesh, int32 LODIndex, FMeshSource& Out, FString& OutError,
		const TArray<int32>* KeepSlots = nullptr)
	{
		Out = FMeshSource();
		Out.LODIndex = LODIndex;
		Out.NativeSource = Mesh;
		const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
		{
			OutError = FString::Printf(TEXT("%s has no LOD %d."), Mesh ? *Mesh->GetName() : TEXT("The mesh"), LODIndex);
			return false;
		}
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
		const FPositionVertexBuffer& PositionBuffer = LOD.StaticVertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& VertexBuffer = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
		const FSkinWeightVertexBuffer& WeightBuffer = LOD.SkinWeightVertexBuffer;
		const int32 NumVertices = static_cast<int32>(PositionBuffer.GetNumVertices());
		if (NumVertices == 0 || !PositionBuffer.GetVertexData() || !VertexBuffer.GetTangentData() || !VertexBuffer.GetTexCoordData()
			|| WeightBuffer.GetNumVertices() != static_cast<uint32>(NumVertices) || !LOD.MultiSizeIndexContainer.IsIndexBufferValid())
		{
			OutError = FString::Printf(TEXT("%s LOD %d has no CPU copy of its vertices, weights or triangles. Open the mesh once in the editor and try again."),
				*Mesh->GetName(), LODIndex);
			return false;
		}
		TArray<uint32> Indices;
		LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);

		// Sections in vertex order: they must tile the vertex buffer for the file's vertex order to be the mesh's.
		const bool bPacked = KeepSlots != nullptr;
		TArray<int32> Order = FMantleCosmeticExport::KeptSections(FMantleCosmeticExport::ReadSectionSpans(Mesh, LODIndex), KeepSlots);
		if (Order.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s LOD %d draws none of the sections chosen for this item."), *Mesh->GetName(), LODIndex);
			return false;
		}
		// A LOD whose reference topology is in its mesh description's order (a catalog reference body, or a body
		// written in its order, FMantleCosmeticExport::HasReferenceTopology): the editor build renumbered its render vertices,
		// so the file takes the reference order, each file vertex the render vertex the bridge names.
		TArray<int32> ReferenceOfRender;
		if (!bPacked)
		{
			const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
			TArray<int32> RenderOfReference;
			bool bDescriptionOrder = false;
			if (Contract && FMantleCosmeticExport::HasReferenceTopology(Mesh, LODIndex, *Contract, &bDescriptionOrder, &RenderOfReference)
				&& bDescriptionOrder && RenderOfReference.Num() == NumVertices)
			{
				ReferenceOfRender.Init(INDEX_NONE, NumVertices);
				for (int32 Reference = 0; Reference < RenderOfReference.Num(); ++Reference)
				{
					ReferenceOfRender[RenderOfReference[Reference]] = Reference;
				}
			}
		}
		const bool bReferenceOrder = ReferenceOfRender.Num() > 0;

		// Where each section's vertices land in the file: where they are in the mesh, or (some slots kept)
		// packed one section after another, or where the reference order puts them.
		TArray<int32> FileFirstVertex;
		FileFirstVertex.Init(INDEX_NONE, LOD.RenderSections.Num());
		int32 Count = 0;
		if (bPacked)
		{
			for (const int32 Section : Order)
			{
				FileFirstVertex[Section] = Count;
				Count += static_cast<int32>(LOD.RenderSections[Section].NumVertices);
			}
		}
		else if (bReferenceOrder)
		{
			for (const int32 Section : Order)
			{
				const FSkelMeshRenderSection& Render = LOD.RenderSections[Section];
				int32 Lowest = MAX_int32;
				int32 Highest = INDEX_NONE;
				for (uint32 Vertex = Render.BaseVertexIndex; Vertex < Render.BaseVertexIndex + Render.NumVertices && Vertex < static_cast<uint32>(NumVertices); ++Vertex)
				{
					Lowest = FMath::Min(Lowest, ReferenceOfRender[static_cast<int32>(Vertex)]);
					Highest = FMath::Max(Highest, ReferenceOfRender[static_cast<int32>(Vertex)]);
				}
				if (Render.NumVertices == 0 || Highest - Lowest + 1 != static_cast<int32>(Render.NumVertices))
				{
					OutError = FString::Printf(TEXT("%s LOD %d's section %d is not one run of the reference order, which Mantle's format needs. Convert the body again."),
						*Mesh->GetName(), LODIndex, Section);
					return false;
				}
				FileFirstVertex[Section] = Lowest;
				Count += static_cast<int32>(Render.NumVertices);
			}
			if (Count != NumVertices)
			{
				OutError = FString::Printf(TEXT("%s LOD %d has vertices outside its sections."), *Mesh->GetName(), LODIndex);
				return false;
			}
			// Sections in the file's vertex order, as the cook reads them.
			Order.StableSort([&FileFirstVertex](const int32 A, const int32 B) { return FileFirstVertex[A] < FileFirstVertex[B]; });
		}
		else
		{
			const int32 FirstVertex = static_cast<int32>(LOD.RenderSections[Order[0]].BaseVertexIndex);
			int32 Covered = FirstVertex;
			for (const int32 Section : Order)
			{
				const FSkelMeshRenderSection& Render = LOD.RenderSections[Section];
				if (static_cast<int32>(Render.BaseVertexIndex) != Covered)
				{
					OutError = FString::Printf(TEXT("%s LOD %d's sections share vertices, which Mantle's format cannot keep in order. Reimport the mesh."),
						*Mesh->GetName(), LODIndex);
					return false;
				}
				FileFirstVertex[Section] = Covered - FirstVertex;
				Covered += static_cast<int32>(Render.NumVertices);
			}
			if (FirstVertex != 0 || Covered != NumVertices)
			{
				OutError = FString::Printf(TEXT("%s LOD %d has vertices outside its sections."), *Mesh->GetName(), LODIndex);
				return false;
			}
			Count = Covered - FirstVertex;
		}
		// The file vertex a section's render vertex lands on.
		auto FileVertexOf = [&LOD, &FileFirstVertex, &ReferenceOfRender, bReferenceOrder](const int32 Section, const uint32 RenderVertex) -> int64
		{
			return bReferenceOrder ? static_cast<int64>(ReferenceOfRender[static_cast<int32>(RenderVertex)])
				: static_cast<int64>(FileFirstVertex[Section]) + static_cast<int64>(RenderVertex) - static_cast<int64>(LOD.RenderSections[Section].BaseVertexIndex);
		};
		// The mesh's vertex each file vertex is.
		TArray<uint32> SourceOfVertex;
		SourceOfVertex.SetNumZeroed(Count);
		for (const int32 Section : Order)
		{
			const FSkelMeshRenderSection& Render = LOD.RenderSections[Section];
			if (Render.BaseVertexIndex + Render.NumVertices > static_cast<uint32>(NumVertices))
			{
				OutError = FString::Printf(TEXT("%s LOD %d has a section past its vertices. Reimport the mesh."), *Mesh->GetName(), LODIndex);
				return false;
			}
			for (uint32 Vertex = 0; Vertex < Render.NumVertices; ++Vertex)
			{
				SourceOfVertex[static_cast<int32>(FileVertexOf(Section, Render.BaseVertexIndex + Vertex))] = Render.BaseVertexIndex + Vertex;
			}
		}

		FMantleGlbMesh& Glb = Out.Glb;
		Glb.Name = Mesh->GetName();
		Glb.Positions.SetNumUninitialized(Count);
		Glb.Normals.SetNumUninitialized(Count);
		Glb.Tangents.SetNumUninitialized(Count);
		Glb.UVs.SetNumUninitialized(Count);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			const uint32 Source = SourceOfVertex[Vertex];
			Glb.Positions[Vertex] = PositionBuffer.VertexPosition(Source);
			const FVector4f TangentX = VertexBuffer.VertexTangentX(Source);
			const FVector4f TangentZ = VertexBuffer.VertexTangentZ(Source);
			Glb.Normals[Vertex] = FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z);
			Glb.Tangents[Vertex] = FVector4f(TangentX.X, TangentX.Y, TangentX.Z, TangentZ.W < 0.0f ? -1.0f : 1.0f);
			Glb.UVs[Vertex] = VertexBuffer.GetVertexUV(Source, 0);
			Out.Bounds += Glb.Positions[Vertex];
		}

		// Materials in the order sections first use them: the cook's section order.
		const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
		TMap<int32, int32> GlbMaterialOfSlot;
		TArray<int32> SectionOfVertex;
		SectionOfVertex.Init(INDEX_NONE, Count);
		for (const int32 Section : Order)
		{
			const FSkelMeshRenderSection& Render = LOD.RenderSections[Section];
			const int32 Slot = SectionSlot(Mesh, LODIndex, Section, Render.MaterialIndex);
			int32* GlbMaterial = GlbMaterialOfSlot.Find(Slot);
			if (!GlbMaterial)
			{
				UMaterialInterface* Material = Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialInterface.Get() : nullptr;
				Glb.MaterialNames.Add(SlotName(Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName : NAME_None, Material, Slot));
				Out.Materials.Add(Material);
				Out.SlotIndices.Add(Slot); // A place skeletal mesh's materials[] follow it
				GlbMaterial = &GlbMaterialOfSlot.Add(Slot, Glb.MaterialNames.Num() - 1);
			}
			FMantleGlbPrimitive& Primitive = Glb.Primitives.AddDefaulted_GetRef();
			Primitive.MaterialIndex = *GlbMaterial;
			Primitive.FirstVertex = FileFirstVertex[Section];
			Primitive.NumVertices = static_cast<int32>(Render.NumVertices);
			const int32 Begin = static_cast<int32>(Render.BaseIndex);
			const int32 End = Begin + static_cast<int32>(Render.NumTriangles) * 3;
			if (End > Indices.Num())
			{
				OutError = FString::Printf(TEXT("%s LOD %d has a section past its index buffer."), *Mesh->GetName(), LODIndex);
				return false;
			}
			for (int32 Index = Begin; Index < End; ++Index)
			{
				const int64 InSection = static_cast<int64>(Indices[Index]) - Render.BaseVertexIndex;
				const int64 Local = InSection < 0 || InSection >= Render.NumVertices ? -1 : FileVertexOf(Section, Indices[Index]) - FileFirstVertex[Section];
				if (Local < 0 || Local >= Render.NumVertices)
				{
					OutError = FString::Printf(TEXT("%s LOD %d has a triangle that uses another section's vertices. Reimport the mesh."),
						*Mesh->GetName(), LODIndex);
					return false;
				}
				Primitive.Indices.Add(static_cast<uint32>(Local));
			}
			Out.NumTriangles += static_cast<int32>(Render.NumTriangles);
			for (uint32 Vertex = 0; Vertex < Render.NumVertices; ++Vertex)
			{
				SectionOfVertex[Primitive.FirstVertex + static_cast<int32>(Vertex)] = Section;
			}
		}

		// All compatible bodies export the same triangle order as well as vertex order. Clearance
		// caches identify triangles by index, so an engine build's reordered faces cannot leak through.
		if (!bPacked && Glb.Primitives.Num() == 1)
		{
			if (const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::Get();
				Layout && Glb.UVs.Num() == Layout->QuantizedUVs.Num()
				&& UMantleBodyTopologyData::ComputeUVHash(Glb.UVs) == Layout->UVHash && !FMantleFitBodies::IsCreatorPreview(Mesh))
			{
				TArray<int32> Identity;
				if (!Layout->Match(Glb.UVs, Glb.Primitives[0].Indices, Identity, OutError))
				{
					return false;
				}
				for (int32 Vertex = 0; Vertex < Identity.Num(); ++Vertex)
				{
					if (Identity[Vertex] != Vertex)
					{
						OutError = TEXT("body attributes were not read in the verified canonical vertex order");
						return false;
					}
				}
				Glb.Primitives[0].Indices = Layout->Indices;
			}
			else if (Layout && Glb.UVs.Num() == Layout->QuantizedUVs.Num() && FMantleFitBodies::IsCreatorPreview(Mesh))
			{
				// Creator's in-memory body, put into the reference order by the UV match: its positions, normals and skin are
				// the reference vertices', and the layout (UVs, triangles) is the reference's own, which is what the backend checks.
				{
					for (int32 Vertex = 0; Vertex < Glb.UVs.Num(); ++Vertex)
					{
						Glb.UVs[Vertex] = FVector2f(Layout->QuantizedUVs[Vertex].X / 2048.0f, Layout->QuantizedUVs[Vertex].Y / 2048.0f);
					}
					if (UMantleBodyTopologyData::ComputeUVHash(Glb.UVs) != Layout->UVHash)
					{
						OutError = TEXT("the reference UV layout could not be restored for Creator's body");
						return false;
					}
					Glb.Primitives[0].Indices = Layout->Indices;
				}
			}
		}

		// The raw reference skeleton, component poses.
		const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
		const TArray<FMeshBoneInfo>& BoneInfo = Reference.GetRawRefBoneInfo();
		const TArray<FTransform>& BonePose = Reference.GetRawRefBonePose();
		Out.ComponentPose.SetNum(BoneInfo.Num());
		Out.Weighted.Init(false, BoneInfo.Num());
		for (int32 Bone = 0; Bone < BoneInfo.Num(); ++Bone)
		{
			FMantleGlbJoint& Joint = Glb.Joints.AddDefaulted_GetRef();
			Joint.Name = BoneInfo[Bone].Name;
			Joint.Parent = BoneInfo[Bone].ParentIndex;
			Joint.Local = BonePose.IsValidIndex(Bone) ? BonePose[Bone] : FTransform::Identity;
			Out.ComponentPose[Bone] = Joint.Parent == INDEX_NONE ? Joint.Local : Joint.Local * Out.ComponentPose[Joint.Parent];
		}

		// Weights: the section's bone map to mesh bones, floats, non-zero only, heaviest first, at most MaxSkinInfluences.
		const bool bSixteenBit = WeightBuffer.Use16BitBoneWeight();
		const int32 MaxSlots = FMath::Clamp(static_cast<int32>(WeightBuffer.GetMaxBoneInfluences()), 1, static_cast<int32>(MAX_TOTAL_INFLUENCES));
		TArray<TArray<TPair<int32, float>>> PerVertex;
		PerVertex.SetNum(Count);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			const int32 Section = SectionOfVertex[Vertex];
			const TConstArrayView<FBoneIndexType> BoneMap = Section != INDEX_NONE
				? TConstArrayView<FBoneIndexType>(LOD.RenderSections[Section].BoneMap)
				: LOD.GetUnifiedBoneMap();
			const FSkinWeightInfo Weights = WeightBuffer.GetVertexSkinWeights(SourceOfVertex[Vertex]);
			for (int32 Slot = 0; Slot < MaxSlots; ++Slot)
			{
				const uint16 Raw = Weights.InfluenceWeights[Slot];
				if (Raw == 0 || !BoneMap.IsValidIndex(Weights.InfluenceBones[Slot]))
				{
					continue;
				}
				const int32 Bone = BoneMap[Weights.InfluenceBones[Slot]];
				const float Weight = bSixteenBit ? Raw / 65535.0f : (Raw >> 8) / 255.0f;
				if (Weight <= 0.0f || !Out.Weighted.IsValidIndex(Bone))
				{
					continue;
				}
				TPair<int32, float>* Existing = PerVertex[Vertex].FindByPredicate([Bone](const TPair<int32, float>& Pair) { return Pair.Key == Bone; });
				if (Existing)
				{
					Existing->Value += Weight;
				}
				else
				{
					PerVertex[Vertex].Add(TPair<int32, float>(Bone, Weight));
				}
			}
			// Heaviest first; past MaxSkinInfluences the lightest are dropped and the rest renormalised.
			const bool bCapped = PerVertex[Vertex].Num() > MaxSkinInfluences;
			const float Dropped = FMantleCosmeticChecks::CapInfluences(PerVertex[Vertex], MaxSkinInfluences);
			if (bCapped)
			{
				++Out.CappedVertices;
				Out.LargestDroppedWeight = FMath::Max(Out.LargestDroppedWeight, Dropped);
			}
			Out.MostInfluences = FMath::Max(Out.MostInfluences, PerVertex[Vertex].Num());
		}
		Glb.NumInfluences = FMath::Clamp(Out.MostInfluences, 1, MaxSkinInfluences);
		Glb.InfluenceJoints.SetNumZeroed(Count * Glb.NumInfluences);
		Glb.InfluenceWeights.SetNumZeroed(Count * Glb.NumInfluences);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			for (int32 Slot = 0; Slot < FMath::Min(Glb.NumInfluences, PerVertex[Vertex].Num()); ++Slot)
			{
				const TPair<int32, float>& Influence = PerVertex[Vertex][Slot];
				Glb.InfluenceJoints[Vertex * Glb.NumInfluences + Slot] = static_cast<uint16>(Influence.Key);
				Glb.InfluenceWeights[Vertex * Glb.NumInfluences + Slot] = Influence.Value;
				Out.Weighted[Influence.Key] = true;
			}
		}
		return true;
	}

	/** One LOD of a static mesh: render vertices in order, one primitive per section when sections tile the vertices. */
	bool ReadStaticLOD(const UStaticMesh* Mesh, int32 LODIndex, FMeshSource& Out, FString& OutError)
	{
		Out = FMeshSource();
		Out.LODIndex = LODIndex;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || !RenderData->LODResources.IsValidIndex(LODIndex))
		{
			OutError = FString::Printf(TEXT("%s has no LOD %d."), Mesh ? *Mesh->GetName() : TEXT("The mesh"), LODIndex);
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
		const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
		const int32 NumVertices = static_cast<int32>(PositionBuffer.GetNumVertices());
		if (NumVertices == 0 || !PositionBuffer.GetVertexData() || !VertexBuffer.GetTangentData() || !VertexBuffer.GetTexCoordData())
		{
			OutError = FString::Printf(TEXT("%s LOD %d has no CPU copy of its vertices. Enable Allow CPU Access on the mesh and try again."),
				*Mesh->GetName(), LODIndex);
			return false;
		}
		TArray<uint32> Indices;
		LOD.IndexBuffer.GetCopy(Indices);
		if (Indices.Num() < 3)
		{
			OutError = FString::Printf(TEXT("%s LOD %d has no triangles."), *Mesh->GetName(), LODIndex);
			return false;
		}

		FMantleGlbMesh& Glb = Out.Glb;
		Glb.Name = Mesh->GetName();
		Glb.Positions.SetNumUninitialized(NumVertices);
		Glb.Normals.SetNumUninitialized(NumVertices);
		Glb.Tangents.SetNumUninitialized(NumVertices);
		Glb.UVs.SetNumUninitialized(NumVertices);
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			Glb.Positions[Vertex] = PositionBuffer.VertexPosition(static_cast<uint32>(Vertex));
			const FVector4f TangentX = VertexBuffer.VertexTangentX(static_cast<uint32>(Vertex));
			const FVector4f TangentZ = VertexBuffer.VertexTangentZ(static_cast<uint32>(Vertex));
			Glb.Normals[Vertex] = FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z);
			Glb.Tangents[Vertex] = FVector4f(TangentX.X, TangentX.Y, TangentX.Z, TangentZ.W < 0.0f ? -1.0f : 1.0f);
			Glb.UVs[Vertex] = VertexBuffer.GetVertexUV(static_cast<uint32>(Vertex), 0);
			Out.Bounds += Glb.Positions[Vertex];
		}

		// The second UV set (UV channel 1, often the lightmap or a livery layout) and the painted vertex
		// colours, when the LOD has them; the cook and the client carry both through.
		if (VertexBuffer.GetNumTexCoords() >= 2)
		{
			Glb.UV1s.SetNumUninitialized(NumVertices);
			for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			{
				Glb.UV1s[Vertex] = VertexBuffer.GetVertexUV(static_cast<uint32>(Vertex), 1);
			}
		}
		const FColorVertexBuffer& ColorBuffer = LOD.VertexBuffers.ColorVertexBuffer;
		if (LOD.bHasColorVertexData && ColorBuffer.GetVertexData() && static_cast<int32>(ColorBuffer.GetNumVertices()) == NumVertices)
		{
			Glb.Colors.SetNumUninitialized(NumVertices);
			bool bAllWhite = true;
			for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			{
				Glb.Colors[Vertex] = ColorBuffer.VertexColor(static_cast<uint32>(Vertex));
				bAllWhite = bAllWhite && Glb.Colors[Vertex] == FColor::White;
			}
			// All white is what no colours means everywhere downstream: not worth four bytes a vertex.
			if (bAllWhite)
			{
				Glb.Colors.Reset();
			}
		}

		const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
		TArray<int32> Order;
		for (int32 Section = 0; Section < LOD.Sections.Num(); ++Section)
		{
			if (LOD.Sections[Section].NumTriangles > 0)
			{
				Order.Add(Section);
			}
		}
		Order.Sort([&LOD](int32 A, int32 B) { return LOD.Sections[A].MinVertexIndex < LOD.Sections[B].MinVertexIndex; });
		bool bTiles = Order.Num() > 0;
		int32 Covered = 0;
		for (const int32 Section : Order)
		{
			const FStaticMeshSection& Render = LOD.Sections[Section];
			bTiles = bTiles && static_cast<int32>(Render.MinVertexIndex) == Covered;
			Covered = static_cast<int32>(Render.MaxVertexIndex) + 1;
		}
		bTiles = bTiles && Covered == NumVertices;

		TMap<int32, int32> GlbMaterialOfSlot;
		auto MaterialFor = [&](int32 Slot) -> int32
		{
			if (const int32* Found = GlbMaterialOfSlot.Find(Slot))
			{
				return *Found;
			}
			UMaterialInterface* Material = Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialInterface.Get() : nullptr;
			Glb.MaterialNames.Add(SlotName(Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName : NAME_None, Material, Slot));
			Out.Materials.Add(Material);
			Out.SlotIndices.Add(Slot);
			return GlbMaterialOfSlot.Add(Slot, Glb.MaterialNames.Num() - 1);
		};

		if (bTiles)
		{
			for (const int32 Section : Order)
			{
				const FStaticMeshSection& Render = LOD.Sections[Section];
				FMantleGlbPrimitive& Primitive = Glb.Primitives.AddDefaulted_GetRef();
				Primitive.MaterialIndex = MaterialFor(Render.MaterialIndex);
				Primitive.FirstVertex = static_cast<int32>(Render.MinVertexIndex);
				Primitive.NumVertices = static_cast<int32>(Render.MaxVertexIndex - Render.MinVertexIndex) + 1;
				for (uint32 Index = Render.FirstIndex; Index < Render.FirstIndex + Render.NumTriangles * 3; ++Index)
				{
					Primitive.Indices.Add(Indices[Index] - Render.MinVertexIndex);
				}
				Out.NumTriangles += static_cast<int32>(Render.NumTriangles);
			}
		}
		else
		{
			// Sections share vertices: with one material, one primitive over every vertex; with several, each
			// section is split off with its own copy of the vertices it uses, so the primitives tile the vertex buffer.
			TSet<int32> SlotsUsed;
			for (const int32 Section : Order)
			{
				SlotsUsed.Add(LOD.Sections[Section].MaterialIndex);
			}
			if (SlotsUsed.Num() > 1)
			{
				TArray<FMantleSharedVertexSection> Split;
				for (const int32 Section : Order)
				{
					const FStaticMeshSection& Render = LOD.Sections[Section];
					FMantleSharedVertexSection& Entry = Split.AddDefaulted_GetRef();
					Entry.FirstIndex = static_cast<int32>(Render.FirstIndex);
					Entry.NumTriangles = static_cast<int32>(Render.NumTriangles);
					Entry.GlbMaterial = MaterialFor(Render.MaterialIndex);
					Out.NumTriangles += Entry.NumTriangles;
				}
				FMantleCosmeticExport::SplitSharedVertexSections(Glb, Indices, Split);
				return true;
			}
			FMantleGlbPrimitive& Primitive = Glb.Primitives.AddDefaulted_GetRef();
			Primitive.MaterialIndex = MaterialFor(Order.Num() > 0 ? LOD.Sections[Order[0]].MaterialIndex : 0);
			Primitive.FirstVertex = 0;
			Primitive.NumVertices = NumVertices;
			for (const int32 Section : Order)
			{
				const FStaticMeshSection& Render = LOD.Sections[Section];
				for (uint32 Index = Render.FirstIndex; Index < Render.FirstIndex + Render.NumTriangles * 3; ++Index)
				{
					Primitive.Indices.Add(Indices[Index]);
				}
				Out.NumTriangles += static_cast<int32>(Render.NumTriangles);
			}
		}
		return true;
	}

	/** The note for Source's vertices capped at MaxSkinInfluences (FMantleCosmeticChecks::CappedInfluencesIssue); none when none were. */
	void NoteCappedInfluences(const FMeshSource& Source, const FString& What, TArray<FMantleCosmeticIssue>& OutIssues)
	{
		if (Source.CappedVertices > 0)
		{
			OutIssues.Add(FMantleCosmeticChecks::CappedInfluencesIssue(What, Source.CappedVertices, Source.LargestDroppedWeight, MaxSkinInfluences));
		}
	}

	/**
	 * Each glTF material's UV0 over all its primitives (FMantleCosmeticChecks::MeasureUVTiles), an error for a
	 * section laid out on UDIM tiles. What leads the section's name ("section", "the face's section").
	 */
	void CheckSectionUVTiles(const FMeshSource& Source, const TCHAR* What, TArray<FMantleCosmeticIssue>& OutIssues)
	{
		const FMantleGlbMesh& Glb = Source.Glb;
		for (int32 Material = 0; Material < Glb.MaterialNames.Num(); ++Material)
		{
			TArray<FVector2f> UVs;
			TArray<uint32> Indices;
			for (const FMantleGlbPrimitive& Primitive : Glb.Primitives)
			{
				if (Primitive.MaterialIndex != Material || Primitive.FirstVertex < 0 || Primitive.FirstVertex + Primitive.NumVertices > Glb.UVs.Num())
				{
					continue;
				}
				const uint32 Base = static_cast<uint32>(UVs.Num());
				UVs.Append(Glb.UVs.GetData() + Primitive.FirstVertex, Primitive.NumVertices);
				for (const uint32 Index : Primitive.Indices)
				{
					Indices.Add(Base + Index);
				}
			}
			const FMantleUVTiles Tiles = FMantleCosmeticChecks::MeasureUVTiles(UVs, Indices);
			FMantleCosmeticChecks::CheckUVTiles(FString::Printf(TEXT("%s \"%s\""), What, *Glb.MaterialNames[Material]), Tiles, OutIssues);
			if (Tiles.Tiles.Num() > 1 && !Tiles.IsUdim())
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_UV %s \"%s\": UVs repeat across %d tiles (%d of %d triangles cross a tile border), not a UDIM layout"),
					What, *Glb.MaterialNames[Material], Tiles.Tiles.Num(), Tiles.CrossingTriangles, Tiles.Triangles);
			}
		}
	}

	/**
	 * Fits Source's skin to Fixture (FMantleCosmeticChecks::FitSkinToSkeleton): keeps the bones on the fixture, leaves
	 * out unweighted helper bones that are not on it (cloth and corrective helpers some exports add) and folds a
	 * weighted bone that is not on it (a jaw, breast, hair or sleeve bone of a Fab character) into its nearest kept
	 * ancestor. OutDropped counts both; Source.FoldedBones lists the folded ones.
	 */
	void FitSkinToFixture(FMeshSource& Source, const FMantleContractSkeleton& Fixture, TArray<FMantleSkinJointName>& OutNames, int32& OutDropped)
	{
		FMantleSkinFit Fit;
		FMantleCosmeticChecks::FitSkinToSkeleton(Source.Glb, Source.ComponentPose, Source.Weighted, Fixture, Fit);
		OutNames = MoveTemp(Fit.Names);
		OutDropped = Fit.Dropped;
		if (Fit.Folded.Num() > 0)
		{
			Source.MostInfluences = Fit.MostInfluences;
		}
		Source.FoldedBones = MoveTemp(Fit.Folded);
	}

	/**
	 * Tries Class's skeletons (FMantleCosmeticChecks::FitSkinToClass) and keeps the best fit: the one that folds the
	 * fewest weighted bones, then the one that leaves out the fewest bones, the class order breaking ties; the fitted
	 * source and skeleton, or the worker's message. Fewest, not first: the UE5 Mannequin shares most of its bones (with
	 * the same parents) with the MetaHuman body, so a Manny export also fits the MetaHuman skeleton once its unweighted
	 * ik_*, interaction and center_of_mass bones are left out; kept, those bones are what makes the worker
	 * (cook.identifySkeleton, class order) identify it as ue5mannequin too. A skin on only the shared bones stays
	 * metahuman on both sides. Extra bones folded on every skeleton count the same on each, so they never tip it.
	 * The UE4 Mannequin comes last in the class order, and only bones on no supported skeleton fold, so a
	 * MetaHuman or UE5 Mannequin body is never refitted as a UE4 Mannequin one by folding its spine_04, spine_05,
	 * neck_02 or metacarpals away; a UE4 Mannequin body fits nothing else (its clavicles and neck hang from spine_03,
	 * its fingers from the hand). The UEFN Mannequin comes after it. A UEFN Mannequin export keeps attach,
	 * props_root, prop_01 and poi (bones no other skeleton has) only there, so it leaves out the fewest bones there, and
	 * kept, those bones are what makes the worker identify it as uefnmannequin too; a body without any of them stays
	 * where it was on both sides (ue5mannequin with its IK bones, else metahuman).
	 */
	const FMantleContractSkeleton* FitToClassSkeleton(FMeshSource& Source, const FMantleContractClass& Class, const FMantleAvatarContract& Contract,
		bool bRequireRoot, int32& OutDropped, FString& OutError)
	{
		FMantleSkinFit Fit;
		const FMantleContractSkeleton* Skeleton = FMantleCosmeticChecks::FitSkinToClass(Source.Glb, Source.ComponentPose, Source.Weighted, Class, Contract,
			bRequireRoot, Fit, OutError);
		if (!Skeleton)
		{
			return nullptr;
		}
		OutDropped = Fit.Dropped;
		if (Fit.Folded.Num() > 0)
		{
			Source.MostInfluences = Fit.MostInfluences;
		}
		Source.FoldedBones = MoveTemp(Fit.Folded);
		return Skeleton;
	}

	/**
	 * FitToClassSkeleton for an avatar body with the optional jaw slot:
	 * the same skeleton choice, and on a skeleton with the slot PickedJaw kept as it, and on a skeleton with the
	 * secondary motion slots, the mapped chains kept as them (FMantleCosmeticChecks::FitAvatarBodyWithSlots). OutFit says
	 * what happened to the pick and the mappings.
	 */
	const FMantleContractSkeleton* FitAvatarBodySkeleton(FMeshSource& Source, const FName PickedJaw, TConstArrayView<FMantleSecondaryMapping> Mappings,
		const FMantleAvatarContract& Contract, int32& OutDropped, FMantleAvatarBodyFit& OutFit, FString& OutError)
	{
		const FMantleContractSkeleton* Skeleton = FMantleCosmeticChecks::FitAvatarBodyWithSlots(Source.Glb, Source.ComponentPose, Source.Weighted,
			PickedJaw, Mappings, Contract.AvatarBody, Contract, OutFit, OutError);
		if (!Skeleton)
		{
			return nullptr;
		}
		FMantleSkinFit& Fit = OutFit.Fit;
		OutDropped = Fit.Dropped;
		if (Fit.Folded.Num() > 0)
		{
			Source.MostInfluences = Fit.MostInfluences;
		}
		Source.FoldedBones = MoveTemp(Fit.Folded);
		return Skeleton;
	}

	/** The note for the bones Source's fit folded into their parents (FMantleCosmeticChecks::FoldedBonesIssue); none when none were. */
	void NoteFoldedBones(const FMeshSource& Source, const FString& What, TArray<FMantleCosmeticIssue>& OutIssues)
	{
		if (Source.FoldedBones.Num() > 0)
		{
			OutIssues.Add(FMantleCosmeticChecks::FoldedBonesIssue(What, Source.FoldedBones));
			for (const FMantleFoldedBone& Folded : Source.FoldedBones)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_FOLDED %s %s -> %s"), What.IsEmpty() ? TEXT("mesh") : *What,
					*Folded.Bone.ToString(), *Folded.Into.ToString());
			}
		}
	}

	/**
	 * A generated lower LOD, read from the reduction's copy with the source mesh's raw skeleton as its joints,
	 * re-skinned onto LOD0's final joint table (after FitToClassSkeleton) by bone name, so every LOD shares the one
	 * skin. A bone LOD0 left out gives its weight to its nearest kept ancestor (the reduction blends LOD0's own
	 * weights, so this is the rare helper bone); weights on one joint are merged, heaviest first. Renamed, when
	 * given, maps a source bone to the name LOD0 gave it (a picked jaw is LOD0's "jaw"), so its weights land on it.
	 */
	void RebindLowerLODToJoints(FMeshSource& Lower, const TArray<FMantleGlbJoint>& Joints, const TMap<FName, FName>* Renamed = nullptr)
	{
		FMantleGlbMesh& Glb = Lower.Glb;
		TArray<int32> Target;
		Target.Init(INDEX_NONE, Glb.Joints.Num());
		for (int32 Bone = 0; Bone < Glb.Joints.Num(); ++Bone)
		{
			for (int32 Ancestor = Bone; Ancestor != INDEX_NONE && Target[Bone] == INDEX_NONE; Ancestor = Glb.Joints[Ancestor].Parent)
			{
				const FName* RenamedTo = Renamed ? Renamed->Find(Glb.Joints[Ancestor].Name) : nullptr;
				const FName Name = RenamedTo ? *RenamedTo : Glb.Joints[Ancestor].Name;
				Target[Bone] = Joints.IndexOfByPredicate([Name](const FMantleGlbJoint& Joint) { return Joint.Name == Name; });
			}
		}
		const int32 Slots = Glb.NumInfluences;
		const int32 NumVertices = Glb.Positions.Num();
		Lower.MostInfluences = 0;
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			TArray<TPair<int32, float>, TInlineAllocator<8>> Influences;
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				const int32 Index = Vertex * Slots + Slot;
				const float Weight = Glb.InfluenceWeights[Index];
				const int32 Joint = Target.IsValidIndex(Glb.InfluenceJoints[Index]) ? FMath::Max(Target[Glb.InfluenceJoints[Index]], 0) : 0;
				if (Weight <= 0.0f)
				{
					continue;
				}
				TPair<int32, float>* Existing = Influences.FindByPredicate([Joint](const TPair<int32, float>& Pair) { return Pair.Key == Joint; });
				if (Existing)
				{
					Existing->Value += Weight;
				}
				else
				{
					Influences.Add(TPair<int32, float>(Joint, Weight));
				}
			}
			Influences.Sort([](const TPair<int32, float>& A, const TPair<int32, float>& B) { return A.Value > B.Value; });
			Lower.MostInfluences = FMath::Max(Lower.MostInfluences, Influences.Num());
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				const int32 Index = Vertex * Slots + Slot;
				Glb.InfluenceJoints[Index] = Influences.IsValidIndex(Slot) ? static_cast<uint16>(Influences[Slot].Key) : 0;
				Glb.InfluenceWeights[Index] = Influences.IsValidIndex(Slot) ? Influences[Slot].Value : 0.0f;
			}
		}
		Glb.Joints = Joints;
	}

	template<typename Element>
	bool ReorderNativeBodyStream(TArray<Element>& Stream, const TArray<int32>& InputOfReference, int32 Stride, FString& Error, int32 InputCount = INDEX_NONE)
	{
		if (Stream.IsEmpty()) return true;
		if (InputCount == INDEX_NONE) InputCount = InputOfReference.Num();
		if (Stride <= 0 || Stream.Num() != InputCount * Stride)
		{
			Error = TEXT("a native body vertex stream does not match its complete layout");
			return false;
		}
		TArray<Element> Input = MoveTemp(Stream);
		Stream.SetNumUninitialized(InputOfReference.Num() * Stride);
		for (int32 Reference = 0; Reference < InputOfReference.Num(); ++Reference)
		{
			for (int32 Slot = 0; Slot < Stride; ++Slot)
			{
				Stream[Reference * Stride + Slot] = Input[InputOfReference[Reference] * Stride + Slot];
			}
		}
		return true;
	}

	bool CanonicalizeNativeBodyLOD(FMeshSource& Mesh, const FMantleBodyCorrespondence& Layout, FString& Error)
	{
		FMantleGlbMesh& Glb = Mesh.Glb;
		// Current complete MetaHuman bodies have one intact skin section. Splitting it
		// into independent primitives would change the canonical indexed vertex stream.
		if (Glb.Primitives.Num() != 1 || Glb.Primitives[0].FirstVertex != 0 || Glb.Primitives[0].NumVertices != Glb.Positions.Num())
		{
			Error = TEXT("a canonical native body LOD must retain its one complete skin primitive");
			return false;
		}
		TArray<int32> InputOfReference;
		if (Layout.bLowerCornerTopology)
		{
			// Lower native packed shading corners are not RBF/source-shape sample IDs.
			// Preserve all their physical streams and indices, including tangent splits.
			return Layout.MatchCorners(Glb.UVs, Glb.Primitives[0].Indices, InputOfReference, Error)
				&& Layout.ValidateCornerCopies(InputOfReference, Glb.Positions, Glb.InfluenceJoints,
					Glb.InfluenceWeights, Glb.NumInfluences, Error, true);
		}
		const int32 InputCount = Glb.Positions.Num();
		TArray<int32> ReferenceOfInput;
		if (!Layout.Match(Glb.UVs, Glb.Primitives[0].Indices, InputOfReference, Error))
		{
			if (Mesh.LODIndex == 0)
			{
				if (!Layout.MatchMergedBodyExport(Mesh.NativeSource, ReferenceOfInput, InputOfReference, Mesh.MergedBodySplits, Error)
					|| ReferenceOfInput.Num() != InputCount) return false;
			}
			else
			{
				// Lower LODs: Unreal splits a vertex at a UV seam when the tangent differs; merge tangent-only twins.
				if (!Layout.MatchTangentSplits(Glb.UVs, Glb.UV1s, Glb.Primitives[0].Indices, Glb.Positions, Glb.Normals, Glb.Colors,
					Glb.InfluenceJoints, Glb.InfluenceWeights, Glb.NumInfluences, ReferenceOfInput, InputOfReference, Error)
					|| ReferenceOfInput.Num() != InputCount) return false;
			}
			for (uint32& Index : Glb.Primitives[0].Indices)
			{
				if (Index >= static_cast<uint32>(ReferenceOfInput.Num())) return false;
				Index = static_cast<uint32>(ReferenceOfInput[Index]);
			}
		}
		if (!ReorderNativeBodyStream(Glb.Positions, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.Normals, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.Tangents, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.UVs, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.UV1s, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.Colors, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.InfluenceJoints, InputOfReference, Glb.NumInfluences, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.InfluenceWeights, InputOfReference, Glb.NumInfluences, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.HairRootUV, InputOfReference, 1, Error, InputCount)
			|| !ReorderNativeBodyStream(Glb.HairCoordU, InputOfReference, 1, Error, InputCount)) return false;
		if (!ReferenceOfInput.IsEmpty())
		{
			TArray<int32> Identity;
			if (!Layout.Match(Glb.UVs, Glb.Primitives[0].Indices, Identity, Error)
				|| UMantleBodyTopologyData::ComputeUVHash(Glb.UVs) != Layout.UVHash) return false;
			for (int32 I = 0; I < Identity.Num(); ++I)
				if (Identity[I] != I) { Error = TEXT("merged body export is not in canonical UV/index order"); return false; }
			Glb.Primitives[0].NumVertices = Glb.Positions.Num();
		}
		Glb.Primitives[0].Indices = Layout.Indices;
		return true;
	}

	bool ReadCanonicalNativeBodyLODs(const USkeletalMesh* Body, FMeshSource& Source, TArray<FMeshSource>& Lower,
		FString& Error, const TMap<FName, FName>* Renamed)
	{
		Lower.Reset();
		for (int32 Level = 0; Level < FMantleBodyCorrespondence::ExpectedNativeLODs; ++Level)
		{
			const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::GetLOD(Level);
			FMeshSource Native;
			FMeshSource* Current = &Source;
			if (!Layout || (Level > 0 && !ReadSkeletalLOD(Body, Source.LODIndex + Level, Native, Error)))
			{
				Error = FString::Printf(TEXT("canonical body LOD%d is missing or unreadable: %s"), Level, *Error);
				return false;
			}
			if (Level > 0) Current = &Native;
			if (!CanonicalizeNativeBodyLOD(*Current, *Layout, Error))
			{
				Error = FString::Printf(TEXT("canonical body LOD%d is incomplete: %s"), Level, *Error);
				return false;
			}
			if (Level > 0)
			{
				RebindLowerLODToJoints(Native, Source.Glb.Joints, Renamed);
				Lower.Add(MoveTemp(Native));
			}
		}
		return true;
	}

	/**
	 * LOD1, LOD2, ... of LOD SourceLOD of Mesh, generated by the editor's skeletal mesh reduction
	 * (IMeshReduction through FLODUtilities::SimplifySkeletalMeshLOD, one FSkeletalMeshOptimizationSettings per
	 * LOD) at Fractions of the source LOD's triangles, with the source LOD's skin weights (up to 8 influences), its
	 * normals and its sections. Runs on a transient duplicate of Mesh: the creator's asset is never touched. Each
	 * generated LOD is read like the source (ReadSkeletalLOD), with the source's KeepSlots. False, with the
	 * reason, when the editor has no reduction or a LOD cannot be generated or read.
	 */
	bool GenerateReducedLODs(const USkeletalMesh* Mesh, const int32 SourceLOD, const TConstArrayView<float> Fractions, TArray<FMeshSource>& OutLODs,
		FString& OutError, const TArray<int32>* KeepSlots = nullptr)
	{
		OutLODs.Reset();
		if (Fractions.Num() == 0)
		{
			return true;
		}
		IMeshReduction* Reduction = FModuleManager::Get().LoadModuleChecked<IMeshReductionManagerModule>(TEXT("MeshReductionInterface")).GetSkeletalMeshReductionInterface();
		if (!Reduction || !Reduction->IsSupported())
		{
			OutError = TEXT("this editor has no skeletal mesh reduction (the Skeletal Mesh Simplifier plugin is off)");
			return false;
		}

		UPackage* Transient = GetTransientPackage();
		USkeletalMesh* Work = DuplicateObject<USkeletalMesh>(Mesh, Transient,
			MakeUniqueObjectName(Transient, USkeletalMesh::StaticClass(), FName(*FString::Printf(TEXT("%s_MantleLODs"), *Mesh->GetName()))));
		if (!Work)
		{
			OutError = FString::Printf(TEXT("%s could not be copied to generate its LODs"), *Mesh->GetName());
			return false;
		}
		Work->ClearFlags(RF_Public | RF_Standalone);
		Work->SetFlags(RF_Transient);
		TArray<USkinnedAsset*> Compiling = { Work };
		ON_SCOPE_EXIT
		{
			FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
			Work->MarkAsGarbage();
		};
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		// The asset's LOD settings (MetaHuman bodies have one) must not reshape the copy's LODs on rebuild.
		Work->SetLODSettings(nullptr);

		const ITargetPlatform* Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
		FSkeletalMeshUpdateContext Context;
		Context.SkeletalMesh = Work;
		TArray<int32> Generated;
		for (const float Fraction : Fractions)
		{
			const int32 Target = Work->GetLODNum();
			FSkeletalMeshLODInfo& Info = Work->AddLODInfo();
			Info.bAllowCPUAccess = true;
			FSkeletalMeshOptimizationSettings& Settings = Info.ReductionSettings;
			Settings = FSkeletalMeshOptimizationSettings();
			Settings.BaseLOD = SourceLOD;
			Settings.TerminationCriterion = SMTC_NumOfTriangles;
			Settings.NumOfTrianglesPercentage = FMath::Clamp(Fraction, 0.01f, 1.0f);
			// Skin weights as the source has them (the contract's limit is 8), the source's normals, no morphs.
			Settings.MaxBonesPerVertex = 8;
			Settings.bRecalcNormals = false;
			Settings.bRemapMorphTargets = false;
			Settings.bLockEdges = false;
			Settings.bEnforceBoneBoundaries = false;
			FLODUtilities::SimplifySkeletalMeshLOD(Context, Target, Platform);
			Generated.Add(Target);
		}
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);

		for (const int32 LODIndex : Generated)
		{
			// FLODUtilities preserves each generated section's source section ordinal in
			// Render.MaterialIndex, but it does not carry the source LODMaterialMap onto the
			// new LODInfo. Recover the proven section-index -> global-slot binding on this
			// transient copy before ReadSkeletalLOD resolves names or GLB material indices.
			const FSkeletalMeshRenderData* SourceRenderData = Mesh->GetResourceForRendering();
			const FSkeletalMeshRenderData* WorkRenderData = Work->GetResourceForRendering();
			if (!SourceRenderData || !SourceRenderData->LODRenderData.IsValidIndex(SourceLOD)
				|| !WorkRenderData || !WorkRenderData->LODRenderData.IsValidIndex(LODIndex))
			{
				OutError = FString::Printf(TEXT("%s could not read the source and reduced section maps for LOD %d."), *Mesh->GetName(), LODIndex);
				OutLODs.Reset();
				return false;
			}
			const FSkeletalMeshLODRenderData& SourceLODData = SourceRenderData->LODRenderData[SourceLOD];
			const FSkeletalMeshLODRenderData& WorkLODData = WorkRenderData->LODRenderData[LODIndex];
			TArray<int32> SourceSectionSlots;
			SourceSectionSlots.Reserve(SourceLODData.RenderSections.Num());
			for (int32 SectionIndex = 0; SectionIndex < SourceLODData.RenderSections.Num(); ++SectionIndex)
			{
				SourceSectionSlots.Add(SectionSlot(Mesh, SourceLOD, SectionIndex, SourceLODData.RenderSections[SectionIndex].MaterialIndex));
			}
			TArray<int32> GeneratedMaterialIndices;
			GeneratedMaterialIndices.Reserve(WorkLODData.RenderSections.Num());
			for (const FSkelMeshRenderSection& Section : WorkLODData.RenderSections)
			{
				GeneratedMaterialIndices.Add(Section.MaterialIndex);
			}
			FSkeletalMeshLODInfo* WorkLODInfo = Work->GetLODInfo(LODIndex);
			if (!WorkLODInfo)
			{
				OutError = FString::Printf(TEXT("%s has no LOD info for reduced LOD %d."), *Mesh->GetName(), LODIndex);
				OutLODs.Reset();
				return false;
			}
			TArray<int32> RestoredMaterialMap;
			if (!FMantleCosmeticExport::BuildReducedLODMaterialMap(SourceSectionSlots, GeneratedMaterialIndices, WorkLODInfo->LODMaterialMap,
				Work->GetMaterials().Num(), RestoredMaterialMap, OutError))
			{
				OutError = FString::Printf(TEXT("%s LOD %d could not preserve its source section bindings: %s"), *Mesh->GetName(), LODIndex, *OutError);
				OutLODs.Reset();
				return false;
			}
			WorkLODInfo->LODMaterialMap = MoveTemp(RestoredMaterialMap);

			FMeshSource Lower;
			// The copy keeps the source's material slots, so KeepSlots names the same sections on every LOD.
			if (!ReadSkeletalLOD(Work, LODIndex, Lower, OutError, KeepSlots))
			{
				OutLODs.Reset();
				return false;
			}
			Lower.LODIndex = LODIndex;
			OutLODs.Add(MoveTemp(Lower));
		}
		return true;
	}

	/**
	 * Generates Source's lower LODs from Mesh's LOD Source.LODIndex (Contract.LODTargetFractions after LOD0) and
	 * re-skins them onto Source's joints: what WriteLODs writes after LOD0. A LOD that would break the worker's rules
	 * (more triangles than the previous LOD, a material LOD0 lacks) ends the list there. When generation fails the
	 * item uploads with one LOD and the creator is told (a warning, not an error: one LOD is a valid upload).
	 * KeepSlots (Source was read with it) keeps the same sections of each generated LOD: the whole LOD is reduced and
	 * the kept sections read from it, so the items split from one outfit share one reduction. Renamed as
	 * RebindLowerLODToJoints takes it.
	 */
	TArray<FMeshSource> MakeLowerLODs(const USkeletalMesh* Mesh, const FMeshSource& Source, const FMantleAvatarContract& Contract, const TCHAR* What,
		FMantleCosmeticPackage& Package, const TArray<int32>* KeepSlots = nullptr, const TMap<FName, FName>* Renamed = nullptr)
	{
		TArray<FMeshSource> Lower;
		TArray<float> Fractions;
		for (int32 Index = 1; Index < Contract.LODTargetFractions.Num() && Index < FMantleGlbWriter::MaxLODs; ++Index)
		{
			Fractions.Add(Contract.LODTargetFractions[Index]);
		}
		const double Start = FPlatformTime::Seconds();
		FString Error;
		if (!GenerateReducedLODs(Mesh, Source.LODIndex, Fractions, Lower, Error, KeepSlots))
		{
			Package.Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("%s: its LODs could not be generated (%s), so it uploads with one LOD."), What, *Error),
				TEXT("It still works; it only costs more to draw far away.")));
			return TArray<FMeshSource>();
		}
		int32 Previous = Source.NumTriangles;
		TArray<FString> Counts = { FString::FromInt(Source.NumTriangles) };
		for (int32 Index = 0; Index < Lower.Num(); ++Index)
		{
			RebindLowerLODToJoints(Lower[Index], Source.Glb.Joints, Renamed);
			const bool bMaterialsKept = !Lower[Index].Glb.MaterialNames.ContainsByPredicate([&Source](const FString& Name)
			{
				return !Source.Glb.MaterialNames.Contains(Name);
			});
			if (Lower[Index].NumTriangles <= 0 || Lower[Index].NumTriangles > Previous || !bMaterialsKept)
			{
				Package.Notes.Add(FString::Printf(TEXT("%s: generated LOD%d kept no smaller copy of LOD0's sections (%d triangles), so it and what follows are left out"),
					What, Index + 1, Lower[Index].NumTriangles));
				Lower.SetNum(Index);
				break;
			}
			Previous = Lower[Index].NumTriangles;
			Counts.Add(FString::FromInt(Lower[Index].NumTriangles));
		}
		Package.Notes.Add(FString::Printf(TEXT("%s LODs: %s triangles (generated in %.0f ms)"), What, *FString::Join(Counts, TEXT(" / ")),
			(FPlatformTime::Seconds() - Start) * 1000.0));
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_LODS %s triangles=%s"), What, *FString::Join(Counts, TEXT("/")));
		return Lower;
	}

	/**
	 * The skin of Body (its reference LOD, fitted to Class's skeleton as a garment is) for a garment skinned to root alone
	 * (FMantleGarmentOutfit::FollowsRootOnly) to take with FMantleGarmentOutfit::TransferWeights.
	 */
	bool ReadBodySkin(USkeletalMesh* Body, const FMantleContractClass& Class, const FMantleAvatarContract& Contract, FMantleGlbMesh& OutSkin, FString& OutError)
	{
		const int32 LOD = FMantleCosmeticExport::FindReferenceLOD(Body, Contract);
		FMeshSource Source;
		int32 Dropped = 0;
		if (LOD == INDEX_NONE || !ReadSkeletalLOD(Body, LOD, Source, OutError) || !FitToClassSkeleton(Source, Class, Contract, false, Dropped, OutError))
		{
			OutError = FString::Printf(TEXT("its skin could not be taken from the body %s%s"), *Body->GetName(), OutError.IsEmpty() ? TEXT("") : *(TEXT(": ") + OutError));
			return false;
		}
		OutSkin = MoveTemp(Source.Glb);
		return true;
	}

	/** LOD0 and the generated lower LODs as the GLB (one mesh per LOD when there are several). */
	bool WriteMeshWithLODs(const FMeshSource& Source, const TArray<FMeshSource>& Lower, TArray<uint8>& OutGlb, FString& OutError,
		const bool bGeometryOnly = false)
	{
		if (Lower.Num() == 0)
		{
			return bGeometryOnly
				? FMantleGlbWriter::WriteGeometryOnly(Source.Glb, OutGlb, OutError)
				: FMantleGlbWriter::Write(Source.Glb, OutGlb, OutError);
		}
		TArray<FMantleGlbMesh> LODs;
		LODs.Add(Source.Glb);
		for (const FMeshSource& LOD : Lower)
		{
			LODs.Add(LOD.Glb);
			LODs.Last().Name = Source.Glb.Name;
		}
		return bGeometryOnly
			? FMantleGlbWriter::WriteGeometryOnlyLODs(LODs, OutGlb, OutError)
			: FMantleGlbWriter::WriteLODs(LODs, OutGlb, OutError);
	}

	/** One exported mesh as the body-offsets builder reads it: LOD0 and the lower LODs' positions, and the skin. */
	FMantleBodyOffsetsBuilder::FTarget OffsetsTarget(const FMeshSource& Source, const TArray<FMeshSource>& Lower)
	{
		FMantleBodyOffsetsBuilder::FTarget Out;
		Out.LODs.Add(Source.Glb.Positions);
		for (const FMeshSource& LOD : Lower)
		{
			Out.LODs.Add(LOD.Glb.Positions);
		}
		for (const FMantleGlbJoint& Joint : Source.Glb.Joints)
		{
			Out.JointNames.Add(Joint.Name);
			Out.Parents.Add(Joint.Parent);
			Out.Locals.Add(Joint.Local);
		}
		return Out;
	}

	/**
	 * The converted avatar's weight, muscle, chest and hips shapes, read off the Creator character it was built from
	 * (FMantleBodyOffsetsBuilder::BuildForCharacter) and set on the body's and the face's GLBs. Never fails the upload: when
	 * the character is gone or the shapes do not line up with the exported meshes, the avatar uploads without them (its
	 * sliders stay off) and the issue says why.
	 */
	void AttachBodyOffsets(const USkeletalMesh* BuiltBody, FMeshSource& Body, const TArray<FMeshSource>& BodyLODs, FMeshSource* Face,
		FMantleCosmeticPackage& Package, TArray<FMantleCosmeticIssue>& Issues)
	{
		static const TCHAR* const SlidersOff = TEXT("The weight, muscle, chest and hips sliders will be off for this avatar");
		FString Message;
		UMetaHumanCharacter* Character = FMantleBodyOffsetsBuilder::FindCharacterForBuild(BuiltBody, Message);
		if (!Character)
		{
			Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("%s: %s."), SlidersOff, *Message),
				TEXT("The rest of the avatar uploads as normal. Convert it again from its MetaHuman Creator character to get them.")));
			return;
		}
		const FMantleBodyOffsetsBuilder::FTarget BodyTarget = OffsetsTarget(Body, BodyLODs);
		const FMantleBodyOffsetsBuilder::FTarget FaceTarget = Face ? OffsetsTarget(*Face, TArray<FMeshSource>()) : FMantleBodyOffsetsBuilder::FTarget();
		TArray<FMantleBodyOffsetShape> BodyShapes;
		TArray<FMantleBodyOffsetShape> FaceShapes;
		switch (FMantleBodyOffsetsBuilder::BuildForCharacter(Character, BuiltBody, BodyTarget, Face ? &FaceTarget : nullptr, BodyShapes, FaceShapes, Message))
		{
		case FMantleBodyOffsetsBuilder::EBuildResult::Built:
			Body.Glb.BodyOffsets = MoveTemp(BodyShapes);
			if (Face)
			{
				Face->Glb.BodyOffsets = MoveTemp(FaceShapes);
			}
			Package.Notes.Add(Message);
			break;
		case FMantleBodyOffsetsBuilder::EBuildResult::Skipped:
			Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(TEXT("%s: %s."), SlidersOff, *Message)));
			break;
		default:
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_OFFSETS failed: %s"), *Message);
			Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("%s: %s."), SlidersOff, *Message),
				TEXT("The rest of the avatar uploads as normal; nothing about the body or face is lost.")));
			break;
		}
	}

	/** Every vertex bound 100% to the body's head bone: the chain root..head of the body's skeleton as the skin. */
	bool BindRigidlyToHead(FMeshSource& Source, const USkeletalMesh* Body, FString& OutError)
	{
		const FReferenceSkeleton& Reference = Body->GetRefSkeleton();
		const TArray<FMeshBoneInfo>& BoneInfo = Reference.GetRawRefBoneInfo();
		const TArray<FTransform>& BonePose = Reference.GetRawRefBonePose();
		int32 Head = INDEX_NONE;
		for (int32 Bone = 0; Bone < BoneInfo.Num(); ++Bone)
		{
			if (BoneInfo[Bone].Name == FName(TEXT("head")))
			{
				Head = Bone;
				break;
			}
		}
		if (Head == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s has no head bone; hair binds to a MetaHuman body's head."), *Body->GetName());
			return false;
		}
		TArray<int32> Chain;
		for (int32 Bone = Head; Bone != INDEX_NONE; Bone = BoneInfo[Bone].ParentIndex)
		{
			Chain.Insert(Bone, 0);
		}
		FMantleGlbMesh& Glb = Source.Glb;
		Glb.Joints.Reset();
		for (int32 Link = 0; Link < Chain.Num(); ++Link)
		{
			FMantleGlbJoint& Joint = Glb.Joints.AddDefaulted_GetRef();
			Joint.Name = BoneInfo[Chain[Link]].Name;
			Joint.Parent = Link - 1;
			Joint.Local = BonePose.IsValidIndex(Chain[Link]) ? BonePose[Chain[Link]] : FTransform::Identity;
		}
		const int32 NumVertices = Glb.Positions.Num();
		Glb.NumInfluences = 1;
		Glb.InfluenceJoints.Init(static_cast<uint16>(Chain.Num() - 1), NumVertices);
		Glb.InfluenceWeights.Init(1.0f, NumVertices);
		Source.MostInfluences = 1;
		// Its own weights are gone, so nothing of them was capped.
		Source.CappedVertices = 0;
		Source.LargestDroppedWeight = 0.0f;
		return true;
	}

	/** True if Name contains any of Words, case-insensitively. */
	bool NameMatches(const FString& Name, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Name.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// What a material's base colour really is (FMantleCosmeticExport::ResolveMaterialTextures)

	/** How a texture sample's UVs reach it. */
	enum class EMaterialSampleUV : uint8
	{
		/** UV channel 0 as the mesh has it. */
		Identity,
		/** UV 0 scaled (a Texture Coordinate node's tiling, or a Multiply or Divide on the way): a detail map. */
		Tiled,
		/** Another channel, or UVs the material computes some other way (a function, a panner, an offset). */
		Other,
	};

	/** The expression Input reaches through reroutes and the static switches Context (the material or its instance) selects. */
	UMaterialExpression* TraceMaterialInput(const FExpressionInput& Input, const UMaterialInterface* Context, int32& OutOutput)
	{
		FExpressionInput Traced = Input.GetTracedInput();
		for (int32 Depth = 0; Depth < 16 && Traced.Expression; ++Depth)
		{
			const UMaterialExpressionStaticSwitchParameter* Switch = Cast<UMaterialExpressionStaticSwitchParameter>(Traced.Expression);
			if (!Switch)
			{
				break;
			}
			bool bValue = Switch->DefaultValue != 0;
			bool bSet = false;
			FGuid Guid;
			if (Context && Context->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Switch->ParameterName)), bSet, Guid))
			{
				bValue = bSet;
			}
			Traced = (bValue ? Switch->A : Switch->B).GetTracedInput();
		}
		OutOutput = Traced.OutputIndex;
		return Traced.Expression;
	}

	/** A texture sample node (a Texture Object Parameter derives from the sample parameter but samples nothing). */
	const UMaterialExpressionTextureSample* AsTextureSample(const UMaterialExpression* Expression)
	{
		const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Expression);
		return Sample && !Sample->IsA<UMaterialExpressionTextureObjectParameter>() ? Sample : nullptr;
	}

	/** The texture a sample reads as Context sets it, and the parameter it reads it by (None for a fixed texture). */
	UTexture* SampledTexture(const UMaterialExpressionTextureSample* Sample, const UMaterialInterface* Context, FName& OutParameter)
	{
		OutParameter = NAME_None;
		auto ParameterValue = [Context](FName Parameter, UTexture* Default)
		{
			UTexture* Value = nullptr;
			if (Context && Context->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter)), Value) && Value)
			{
				return Value;
			}
			return Default;
		};
		if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Sample))
		{
			OutParameter = Parameter->ParameterName;
			return ParameterValue(Parameter->ParameterName, Parameter->Texture);
		}
		int32 Output = 0;
		if (const UMaterialExpression* Object = TraceMaterialInput(Sample->TextureObject, Context, Output))
		{
			if (const UMaterialExpressionTextureObjectParameter* Parameter = Cast<UMaterialExpressionTextureObjectParameter>(Object))
			{
				OutParameter = Parameter->ParameterName;
				return ParameterValue(Parameter->ParameterName, Parameter->Texture);
			}
			const UMaterialExpressionTextureObject* Fixed = Cast<UMaterialExpressionTextureObject>(Object);
			return Fixed ? Fixed->Texture.Get() : nullptr;
		}
		return Sample->Texture;
	}

	EMaterialSampleUV ClassifySampleUV(const UMaterialExpressionTextureSample* Sample, const UMaterialInterface* Context)
	{
		int32 Output = 0;
		const UMaterialExpression* UVs = TraceMaterialInput(Sample->Coordinates, Context, Output);
		if (!UVs)
		{
			return Sample->ConstCoordinate == 0 ? EMaterialSampleUV::Identity : EMaterialSampleUV::Other;
		}
		if (const UMaterialExpressionTextureCoordinate* Coordinate = Cast<UMaterialExpressionTextureCoordinate>(UVs))
		{
			if (Coordinate->CoordinateIndex != 0)
			{
				return EMaterialSampleUV::Other;
			}
			const bool bUntiled = FMath::IsNearlyEqual(Coordinate->UTiling, 1.0f) && FMath::IsNearlyEqual(Coordinate->VTiling, 1.0f)
				&& !Coordinate->UnMirrorU && !Coordinate->UnMirrorV;
			return bUntiled ? EMaterialSampleUV::Identity : EMaterialSampleUV::Tiled;
		}
		if (UVs->IsA<UMaterialExpressionMultiply>() || UVs->IsA<UMaterialExpressionDivide>())
		{
			return EMaterialSampleUV::Tiled;
		}
		return EMaterialSampleUV::Other;
	}

	/** A texture the upload can read as one image: a 2D texture with source pixels, one block, one slice. */
	bool IsOneImageTexture(const UTexture* Texture)
	{
		return Cast<UTexture2D>(Texture) && Texture->Source.IsValid() && Texture->Source.GetNumBlocks() == 1 && Texture->Source.GetNumSlices() == 1;
	}

	/** The texture a material input is exactly (one untiled RGB sample on UV 0 of one image), else null with why not. */
	UTexture* PlainInputTexture(const FExpressionInput& Input, const UMaterialInterface* Material, const TCHAR* What, FString& OutWhy)
	{
		int32 Output = 0;
		const UMaterialExpression* Expression = TraceMaterialInput(Input, Material, Output);
		const UMaterialExpressionTextureSample* Sample = AsTextureSample(Expression);
		if (!Expression)
		{
			OutWhy = FString::Printf(TEXT("nothing drives its %s"), What);
			return nullptr;
		}
		if (!Sample)
		{
			FString Node = Expression->GetClass()->GetName();
			Node.RemoveFromStart(TEXT("MaterialExpression"));
			OutWhy = FString::Printf(TEXT("its %s is computed in the material (%s), such as a tint, blend or mask over its textures"), What, *Node);
			return nullptr;
		}
		FName Parameter;
		UTexture* Texture = SampledTexture(Sample, Material, Parameter);
		const EMaterialSampleUV UVs = ClassifySampleUV(Sample, Material);
		// Texture sample outputs: 0 RGB, 1 to 4 one channel, 5 RGBA.
		if (Output != 0 && Output != 5)
		{
			OutWhy = FString::Printf(TEXT("its %s is one channel of a texture"), What);
		}
		else if (UVs == EMaterialSampleUV::Tiled)
		{
			OutWhy = FString::Printf(TEXT("its %s texture tiles over the UVs (a detail map)"), What);
		}
		else if (UVs == EMaterialSampleUV::Other)
		{
			OutWhy = FString::Printf(TEXT("its %s texture is sampled on UVs the material computes"), What);
		}
		else if (!Texture)
		{
			OutWhy = FString::Printf(TEXT("its %s texture is missing"), What);
		}
		else if (!IsOneImageTexture(Texture))
		{
			OutWhy = FString::Printf(TEXT("its %s texture %s is a UDIM, layered or generated texture"), What, *Texture->GetName());
		}
		else if (FMantleCosmeticExport::IsAnimatedMaskTexture(Texture->GetName()))
		{
			OutWhy = FString::Printf(TEXT("its %s texture %s is an animated wrinkle map"), What, *Texture->GetName());
		}
		else
		{
			return Texture;
		}
		return nullptr;
	}

	/** True for an instance of Mantle's own M_MantlePlace (what the converter writes): its textures are read by name, as they are. */
	bool IsMantlePlaceMaterial(UMaterialInterface* Material)
	{
		UMaterialInterface* Permutation = Cast<UMaterialInterface>(FMantlePlaceMaterials::PermutationPath(EMantlePlaceAlphaMode::Opaque, false).TryLoad());
		const UMaterial* MantleBase = Permutation ? Permutation->GetBaseMaterial() : nullptr;
		return MantleBase && Material && Material->GetBaseMaterial() == MantleBase;
	}

	struct FSectionTextures
	{
		UTexture* Albedo = nullptr;
		UTexture* Normal = nullptr;
		UTexture* Orm = nullptr;
		/** A separate opacity texture (hair cards' T_*_alpha); hair packs it into the albedo's alpha. */
		UTexture* Opacity = nullptr;
		TArray<uint8> BakedAlbedo;
		TArray<uint8> BakedNormal;
		FLinearColor FlatColour = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
		bool bFlatColourFound = false;
	};

	/**
	 * Drop whole cards within each section, compact every retained attribute; material mappings stay fixed. Each section
	 * keeps at least its smallest card and the kept cards are spread evenly over the style. When Warnings is given, a pack that
	 * cannot fit even then keeps that smallest fit and says so there; without it (the lower LODs) it fails as before.
	 */
	bool ReduceHairCards(FMeshSource& Mesh, int32 Budget, FString& Error, TArray<FString>* Warnings = nullptr)
	{
		if (Mesh.NumTriangles <= Budget) return true;
		const FMantleGlbMesh Original = Mesh.Glb;
		FMantleGlbMesh Reduced = Original;
		Reduced.Positions.Reset(); Reduced.Normals.Reset(); Reduced.Tangents.Reset(); Reduced.UVs.Reset();
		Reduced.UV1s.Reset(); Reduced.Colors.Reset(); Reduced.InfluenceJoints.Reset(); Reduced.InfluenceWeights.Reset();
		Reduced.HairCoordU.Reset(); Reduced.HairRootUV.Reset(); Reduced.Primitives.Reset();
		TArray<int32> SectionTriangles, SectionSmallest;
		for (const FMantleGlbPrimitive& Part : Original.Primitives)
		{
			TArray<uint32> Absolute; for (uint32 I : Part.Indices) Absolute.Add(I + Part.FirstVertex);
			SectionTriangles.Add(Part.Indices.Num() / 3);
			SectionSmallest.Add(FMantleHairCards::SmallestCardTriangles(Original.Positions, Absolute));
		}
		bool bOverBudget = false;
		const TArray<int32> PartBudgets = FMantleHairCards::AllocateSectionBudgets(SectionTriangles, SectionSmallest, Budget, bOverBudget);
		if (bOverBudget && !Warnings) { Error = TEXT("a hair section cannot fit its budget without splitting a card; provide a coarser authored cards LOD"); return false; }
		int32 Kept = 0, PartIndex = 0;
		for (const FMantleGlbPrimitive& Part : Original.Primitives)
		{
			TArray<uint32> Absolute; for (uint32 I : Part.Indices) Absolute.Add(I + Part.FirstVertex);
			const auto Triangles = FMantleHairCards::SelectWholeCards(Original.Positions, Absolute, PartBudgets[PartIndex++]);
			if (Triangles.IsEmpty()) { Error = TEXT("a hair section cannot fit its budget without splitting a card; provide a coarser authored cards LOD"); return false; }
			auto& Out = Reduced.Primitives.AddDefaulted_GetRef(); Out.MaterialIndex = Part.MaterialIndex; Out.FirstVertex = Reduced.Positions.Num();
			TMap<uint32, uint32> Remap;
			for (int32 T : Triangles) for (int32 C = 0; C < 3; ++C)
			{
				const uint32 V = Absolute[T * 3 + C]; uint32* Existing = Remap.Find(V);
				if (!Existing)
				{
					Existing = &Remap.Add(V, Reduced.Positions.Num() - Out.FirstVertex);
					Reduced.Positions.Add(Original.Positions[V]); Reduced.Normals.Add(Original.Normals[V]); Reduced.UVs.Add(Original.UVs[V]);
					if (!Original.Tangents.IsEmpty()) Reduced.Tangents.Add(Original.Tangents[V]);
					if (!Original.UV1s.IsEmpty()) Reduced.UV1s.Add(Original.UV1s[V]);
					if (!Original.Colors.IsEmpty()) Reduced.Colors.Add(Original.Colors[V]);
					if (!Original.HairCoordU.IsEmpty()) { Reduced.HairCoordU.Add(Original.HairCoordU[V]); Reduced.HairRootUV.Add(Original.HairRootUV[V]); }
					for (int32 K = 0; K < Original.NumInfluences; ++K) { Reduced.InfluenceJoints.Add(Original.InfluenceJoints[V * Original.NumInfluences + K]); Reduced.InfluenceWeights.Add(Original.InfluenceWeights[V * Original.NumInfluences + K]); }
				}
				Out.Indices.Add(*Existing);
			}
			Out.NumVertices = Reduced.Positions.Num() - Out.FirstVertex; Kept += Triangles.Num();
		}
		Mesh.Glb = MoveTemp(Reduced); Mesh.NumTriangles = Kept; Mesh.Bounds = FBox3f(ForceInit);
		for (const auto P : Mesh.Glb.Positions) Mesh.Bounds += P;
		if (Warnings && Kept > Budget)
		{
			Warnings->Add(FString::Printf(TEXT("this hair cannot be made smaller than %d triangles without splitting a card (the limit is %d), so it keeps its smallest fit of at least one card from each section."), Kept, Budget));
		}
		return true;
	}

	/** The image a glTF texture reference (material.<Path>.index) points at, when it is embedded. */
	bool ReadGlbTextureImage(const FMantleGlbDocument& Doc, const TSharedPtr<FJsonObject>& TextureRef, TArray<uint8>& OutBytes)
	{
		double TextureIndex = -1.0;
		if (!TextureRef.IsValid() || !TextureRef->TryGetNumberField(TEXT("index"), TextureIndex))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
		if (!Doc.Json->TryGetArrayField(TEXT("textures"), Textures) || !Doc.Json->TryGetArrayField(TEXT("images"), Images)
			|| !Textures->IsValidIndex(static_cast<int32>(TextureIndex)))
		{
			return false;
		}
		double ImageIndex = -1.0, ViewIndex = -1.0;
		const TSharedPtr<FJsonObject> Texture = (*Textures)[static_cast<int32>(TextureIndex)]->AsObject();
		if (!Texture.IsValid() || !Texture->TryGetNumberField(TEXT("source"), ImageIndex) || !Images->IsValidIndex(static_cast<int32>(ImageIndex)))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Image = (*Images)[static_cast<int32>(ImageIndex)]->AsObject();
		return Image.IsValid() && Image->TryGetNumberField(TEXT("bufferView"), ViewIndex) && Doc.ReadBufferView(static_cast<int32>(ViewIndex), OutBytes);
	}

	/**
	 * GLTFExporter's material bake: the material's base colour and normal flattened to PNGs, for a
	 * material whose textures are not plain parameters.
	 */
	bool BakeMaterialWithGltfExporter(UMaterialInterface* Material, TArray<uint8>& OutAlbedo, TArray<uint8>& OutNormal, FString& OutNote,
		const bool bAutoDetectSize = true)
	{
		UGLTFExportOptions* Options = NewObject<UGLTFExportOptions>();
		Options->ResetToDefault();
		Options->bExportPreviewMesh = false;
		Options->BakeMaterialInputs = EGLTFMaterialBakeMode::Simple;
		Options->DefaultMaterialBakeSize.X = 1024;
		Options->DefaultMaterialBakeSize.Y = 1024;
		Options->DefaultMaterialBakeSize.bAutoDetect = bAutoDetectSize;
		Options->TextureImageFormat = EGLTFTextureImageFormat::PNG;
		Options->bAdjustNormalmaps = false;
		Options->bExportVertexColors = false;
		Options->bExportLights = false;
		Options->bExportCameras = false;

		FString SafeName = Material->GetName();
		for (TCHAR& Character : SafeName)
		{
			Character = FChar::IsAlnum(Character) ? Character : TEXT('_');
		}
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mantle"), TEXT("CosmeticBake"), SafeName + TEXT(".glb")));
		FGLTFExportMessages Messages;
		if (!UGLTFExporter::ExportToGLTF(Material, Path, Options, TSet<AActor*>(), Messages))
		{
			OutNote = FString::Printf(TEXT("GLTFExporter could not bake %s%s"), *Material->GetName(),
				Messages.Errors.Num() > 0 ? *(TEXT(": ") + Messages.Errors[0]) : TEXT(""));
			return false;
		}
		TArray<uint8> Bytes;
		FMantleGlbDocument Doc;
		FString Error;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || !FMantleGlbDocument::Parse(Bytes, Doc, Error))
		{
			OutNote = FString::Printf(TEXT("GLTFExporter's bake of %s could not be read (%s)"), *Material->GetName(), *Error);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
		if (!Doc.Json->TryGetArrayField(TEXT("materials"), Materials) || Materials->Num() == 0)
		{
			OutNote = FString::Printf(TEXT("GLTFExporter wrote no material for %s"), *Material->GetName());
			return false;
		}
		const TSharedPtr<FJsonObject> Baked = (*Materials)[0]->AsObject();
		const TSharedPtr<FJsonObject>* Pbr = nullptr;
		const TSharedPtr<FJsonObject>* BaseColour = nullptr;
		const TSharedPtr<FJsonObject>* Normal = nullptr;
		if (Baked.IsValid() && Baked->TryGetObjectField(TEXT("pbrMetallicRoughness"), Pbr) && Pbr
			&& (*Pbr)->TryGetObjectField(TEXT("baseColorTexture"), BaseColour) && BaseColour)
		{
			ReadGlbTextureImage(Doc, *BaseColour, OutAlbedo);
		}
		if (Baked.IsValid() && Baked->TryGetObjectField(TEXT("normalTexture"), Normal) && Normal)
		{
			ReadGlbTextureImage(Doc, *Normal, OutNormal);
		}
		OutNote = FString::Printf(TEXT("baked %s through GLTFExporter (%s)"), *Material->GetName(),
			OutAlbedo.Num() > 0 ? TEXT("base colour") : TEXT("no base colour texture"));
		// The bake's size follows the textures the material samples. A pattern the material draws itself over a tiny
		// texture (a 16 x 16 blank decal under a plaid cap) then bakes to a smear: bake those at the default size instead.
		const int32 BakedWidth = OutAlbedo.Num() >= 24 && OutAlbedo[1] == 'P' && OutAlbedo[2] == 'N' && OutAlbedo[3] == 'G'
			? (OutAlbedo[16] << 24) | (OutAlbedo[17] << 16) | (OutAlbedo[18] << 8) | OutAlbedo[19] : 0;
		if (bAutoDetectSize && BakedWidth > 0 && BakedWidth < 256)
		{
			TArray<uint8> FullAlbedo, FullNormal;
			FString Note;
			if (BakeMaterialWithGltfExporter(Material, FullAlbedo, FullNormal, Note, false))
			{
				OutAlbedo = MoveTemp(FullAlbedo);
				OutNormal = MoveTemp(FullNormal);
				OutNote = FString::Printf(TEXT("%s at %d x %d, its own textures being only %d px"), *Note, Options->DefaultMaterialBakeSize.X,
					Options->DefaultMaterialBakeSize.Y, BakedWidth);
			}
		}
		return OutAlbedo.Num() > 0;
	}

	/**
	 * Finds what a PBR material has for a section: base colour, normal and (avatars) ORM, by
	 * parameter name, then by the textures the material uses, then by GLTFExporter's bake, then a
	 * flat colour.
	 */
	FSectionTextures ResolveSectionTextures(UMaterialInterface* Material, TArray<FString>& OutNotes, bool bPickOpacity = false)
	{
		FSectionTextures Out;
		if (!Material)
		{
			OutNotes.Add(TEXT("a section has no material: it gets a flat grey"));
			return Out;
		}

		// By the material's graph and parameter names, never an animated wrinkle map or a tiled detail map.
		const FMantleMaterialTextures Found = FMantleCosmeticExport::ResolveMaterialTextures(Material, bPickOpacity);
		Out.Albedo = Found.BaseColor;
		Out.Normal = Found.Normal;
		// An SRMF map (specular, roughness, metallic, fuzz) is not an ORM; the converter's bake reads it, the upload does not.
		Out.Orm = Found.OrmParameter.Contains(TEXT("SRMF"), ESearchCase::IgnoreCase) ? nullptr : Found.Orm;
		Out.Opacity = Found.Opacity;
		if (Out.Albedo && Found.bBaseColorFromLargest)
		{
			OutNotes.Add(FString::Printf(TEXT("%s: base colour taken from %s (largest colour texture it uses)"), *Material->GetName(), *Out.Albedo->GetName()));
		}
		if (!Found.bPlain && Out.Albedo)
		{
			// A computed or layered colour (material attributes, tints, masks) is not its one texture; the
			// picked texture can be an AO, mask or stitch map. Bake the whole material when a renderer is here.
			FString Note;
			if (!GUsingNullRHI && BakeMaterialWithGltfExporter(Material, Out.BakedAlbedo, Out.BakedNormal, Note))
			{
				OutNotes.Add(FString::Printf(TEXT("%s: %s, so it was %s instead of using %s"),
					*Material->GetName(), *Found.Why, *Note, *Out.Albedo->GetName()));
				Out.Albedo = nullptr;
			}
			else
			{
				Out.BakedAlbedo.Reset();
				Out.BakedNormal.Reset();
				OutNotes.Add(FString::Printf(TEXT("%s: %s, so %s is only part of its colour (%s)"),
					*Material->GetName(), *Found.Why, *Out.Albedo->GetName(), GUsingNullRHI ? TEXT("no renderer here to bake it") : *Note));
			}
		}

		if (!Out.Albedo && GUsingNullRHI)
		{
			OutNotes.Add(FString::Printf(TEXT("%s: no renderer here to bake its base colour through GLTFExporter"), *Material->GetName()));
		}
		else if (!Out.Albedo)
		{
			FString Note;
			if (BakeMaterialWithGltfExporter(Material, Out.BakedAlbedo, Out.BakedNormal, Note))
			{
				OutNotes.Add(Material->GetName() + TEXT(": ") + Note);
			}
			else
			{
				OutNotes.Add(Note);
			}
		}

		if (!Out.Albedo && Out.BakedAlbedo.Num() == 0)
		{
			for (const TCHAR* Name : { TEXT("BaseColor"), TEXT("Base Color"), TEXT("Color"), TEXT("Tint") })
			{
				FLinearColor Colour;
				if (Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Colour))
				{
					Out.FlatColour = Colour;
					Out.bFlatColourFound = true;
					break;
				}
			}
		}
		return Out;
	}

	/** Pixels scaled down in place to MaxSize on their longer side when larger (alpha kept). */
	void ScalePixelsWithin(TArray<FColor>& Pixels, FIntPoint& Size, int32 MaxSize, bool bLinear)
	{
		if (MaxSize <= 0 || FMath::Max(Size.X, Size.Y) <= MaxSize)
		{
			return;
		}
		const float Scale = static_cast<float>(MaxSize) / FMath::Max(Size.X, Size.Y);
		const int32 NewWidth = FMath::Max(1, FMath::RoundToInt(Size.X * Scale));
		const int32 NewHeight = FMath::Max(1, FMath::RoundToInt(Size.Y * Scale));
		TArray<FColor> Resized;
		Resized.SetNumUninitialized(NewWidth * NewHeight);
		FImageUtils::ImageResize(Size.X, Size.Y, TArrayView<const FColor>(Pixels), NewWidth, NewHeight, TArrayView<FColor>(Resized), !bLinear, false);
		Pixels = MoveTemp(Resized);
		Size = FIntPoint(NewWidth, NewHeight);
	}

	/** A PNG from another encoder (a bake) as BGRA pixels, scaled down to MaxSize. */
	bool DecodePngPixels(const TArray<uint8>& Png, int32 MaxSize, bool bLinear, TArray<FColor>& OutPixels, FIntPoint& OutSize)
	{
		FImage Image;
		if (!FImageUtils::DecompressImage(Png.GetData(), Png.Num(), Image))
		{
			return false;
		}
		// Data/normal PNG bytes are linear channels under the cook contract, regardless of PNG gamma metadata.
		if (bLinear) Image.GammaSpace = EGammaSpace::Linear;
		FImage Bgra;
		Image.CopyTo(Bgra, ERawImageFormat::BGRA8, bLinear ? EGammaSpace::Linear : EGammaSpace::sRGB);
		OutSize = FIntPoint(Bgra.SizeX, Bgra.SizeY);
		OutPixels = TArray<FColor>(Bgra.AsBGRA8().GetData(), static_cast<int32>(Bgra.AsBGRA8().Num()));
		ScalePixelsWithin(OutPixels, OutSize, MaxSize, bLinear);
		return OutPixels.Num() > 0;
	}

	/** A PNG from another encoder (a bake) scaled to MaxSize and re-encoded, so every upload is the plugin's own RGBA PNG. */
	bool NormalizePng(const TArray<uint8>& Png, int32 MaxSize, bool bLinear, TArray<uint8>& OutPng, FIntPoint& OutSize)
	{
		TArray<FColor> Pixels;
		if (!DecodePngPixels(Png, MaxSize, bLinear, Pixels, OutSize))
		{
			return false;
		}
		OutPng = FMantlePng::EncodeRGBA8(OutSize.X, OutSize.Y, Pixels);
		return OutPng.Num() > 0;
	}

	/**
	 * A texture's source pixels as BGRA (sRGB colour, or linear data when bLinear), scaled down to MaxSize on the longer
	 * side when larger: what TextureToPng encodes. A UDIM texture gives its tile 1001. False with a reason for a texture
	 * with no source, a UDIM texture without tile 1001, or a cube or array texture.
	 */
	bool ReadTexturePixels(const UTexture* Texture, int32 MaxSize, bool bLinear, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		OutPixels.Reset();
		if (!Texture || !Texture->Source.IsValid())
		{
			OutError = FString::Printf(TEXT("%s has no source pixels (a cooked or generated texture)"), Texture ? *Texture->GetName() : TEXT("the texture"));
			return false;
		}
		// A UDIM texture gives its tile 1001, the only one a section on 0..1 UVs (all the upload takes) samples.
		int32 Block = 0;
		if (Texture->Source.GetNumBlocks() > 1)
		{
			Block = INDEX_NONE;
			for (int32 Candidate = 0; Candidate < Texture->Source.GetNumBlocks() && Block == INDEX_NONE; ++Candidate)
			{
				FTextureSourceBlock Info;
				Texture->Source.GetBlock(Candidate, Info);
				Block = Info.BlockX == 0 && Info.BlockY == 0 ? Candidate : INDEX_NONE;
			}
		}
		if (Block == INDEX_NONE || Texture->Source.GetNumSlices() > 1)
		{
			OutError = FString::Printf(TEXT("%s is a UDIM, cube or array texture; Mantle takes one 2D image"), *Texture->GetName());
			return false;
		}
		FImage Image;
		if (!const_cast<UTexture*>(Texture)->Source.GetMipImage(Image, Block, 0, 0))
		{
			OutError = FString::Printf(TEXT("%s's pixels could not be read"), *Texture->GetName());
			return false;
		}
		FImage Bgra;
		Image.CopyTo(Bgra, ERawImageFormat::BGRA8, bLinear ? EGammaSpace::Linear : EGammaSpace::sRGB);
		OutSize = FIntPoint(Bgra.SizeX, Bgra.SizeY);
		const TArrayView64<FColor> View = Bgra.AsBGRA8();
		OutPixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
		if (OutPixels.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no pixels"), *Texture->GetName());
			return false;
		}
		ScalePixelsWithin(OutPixels, OutSize, MaxSize, bLinear);
		return true;
	}

	enum class ETextureUse : uint8 { Colour, Normal, Data };

	bool NormalizeGroomAtlas(const FMantleGroomCardsEntry& Entry, int32 MaxSize, TArray<uint8>& Png, FString& Description, FString& Error)
	{
		const int32 Count = Entry.TextureLayout == 2 ? 2 : Entry.TextureLayout == 3 ? 3 : 4;
		if (Entry.TextureLayout < 0 || Entry.TextureLayout > 3 || Entry.Textures.Num() < Count)
		{ Error = TEXT("the groom's cards texture layout or fixed atlas slots are incomplete"); return false; }
		TArray<TArray<FColor>> Pixels; Pixels.SetNum(Count); TArray<FIntPoint> Sizes; Sizes.SetNumZeroed(Count);
		for (int32 I = 0; I < Count; ++I)
		{
			// Layout0/1 tangent texture is unused; compact layouts carry native CoordU in tangent alpha.
			if (Entry.TextureLayout <= 1 && I == 2) continue;
			FIntPoint Current;
			if (!ReadTexturePixels(Entry.Textures[I].Get(), MaxSize, true, Pixels[I], Current, Error)) return false;
			Sizes[I] = Current;
		}
		// Native compact cards use a lower-resolution tangent/CoordU atlas. Sample all
		// channels in the same atlas UV domain, keeping the coverage image resolution.
		const FIntPoint Size = Sizes[Entry.TextureLayout == 3 ? 2 : 1];
		for (int32 I = 0; I < Count; ++I)
		{
			if (Pixels[I].IsEmpty() || Sizes[I] == Size) continue;
			TArray<FColor> Resized; Resized.SetNumUninitialized(Size.X * Size.Y);
			FImageUtils::ImageResize(Sizes[I].X, Sizes[I].Y, TArrayView<const FColor>(Pixels[I]), Size.X, Size.Y, TArrayView<FColor>(Resized), false, false);
			Pixels[I] = MoveTemp(Resized);
		}
		int32 CoordMin = 255, CoordMax = 0, CoverageMin = 255, CoverageMax = 0;
		TArray<FColor> Packed; Packed.SetNumUninitialized(Size.X * Size.Y);
		for (int32 P = 0; P < Packed.Num(); ++P)
		{
			TArray<FColor, TInlineAllocator<4>> Values; Values.Init(FColor::Black, Count);
			for (int32 I = 0; I < Count; ++I) if (Pixels[I].IsValidIndex(P)) Values[I] = Pixels[I][P];
			Packed[P] = FMantleHairCards::AtlasPixel(Entry.TextureLayout, Values, true);
			const uint8 CoordU = Entry.TextureLayout <= 1 ? Values[3].B : Values[0].A;
			if (Packed[P].A != CoordU)
			{ Error = TEXT("native atlas normalization did not preserve the resampled source CoordU channel"); return false; }
			CoordMin = FMath::Min(CoordMin, int32(CoordU)); CoordMax = FMath::Max(CoordMax, int32(CoordU));
			CoverageMin = FMath::Min(CoverageMin, int32(Packed[P].R)); CoverageMax = FMath::Max(CoverageMax, int32(Packed[P].R));
		}
		Png = FMantlePng::EncodeRGBA8(Size.X, Size.Y, Packed);
		TArray<FColor> RoundTrip; FIntPoint RoundTripSize;
		if (!DecodePngPixels(Png, MaxSize, true, RoundTrip, RoundTripSize) || RoundTripSize != Size || RoundTrip != Packed)
		{ Error = TEXT("native atlas PNG did not retain all four normalized data channels"); return false; }
		Description = FString::Printf(TEXT("native atlas group=%d layout=%d size=%dx%d coverageRange=%d..%d CoordUAlphaRange=%d..%d PNGfourChannelExact=true"),
			Entry.GroupIndex, Entry.TextureLayout, Size.X, Size.Y, CoverageMin, CoverageMax, CoordMin, CoordMax);
		return true;
	}

	/** The API's per-texture upload cap: backend/internal/assets MaxScreenedImageBytes (the image classifier's limit). */
	constexpr int32 MaxCosmeticTextureBytes = 8 << 20;

	/** Uploads a texture once however many sections use it: the key is the texture and its role. */
	FString AddTexture(FMantleCosmeticPackage& Package, UTexture* Texture, const TArray<uint8>* Baked, ETextureUse Use, int32 MaxSize,
		const FString& What, const FMantleAvatarContract& Contract)
	{
		const TCHAR* Role = Use == ETextureUse::Colour ? TEXT("color") : Use == ETextureUse::Normal ? TEXT("normal") : TEXT("data");
		const FString Key = FString::Printf(TEXT("texture.%s.%s"), Texture ? *Texture->GetPathName() : *(TEXT("baked.") + What), Role);
		if (Package.FindAsset(Key))
		{
			return Key;
		}
		TArray<uint8> Png;
		FIntPoint Size = FIntPoint::ZeroValue;
		FString Error;
		int32 Limit = FMath::Min(MaxSize > 0 ? MaxSize : Contract.MaxTextureSize, Contract.MaxTextureSize);
		const auto Encode = [&]()
		{
			if (!Texture)
			{
				return Baked && NormalizePng(*Baked, Limit, Use != ETextureUse::Colour, Png, Size);
			}
			if (!FMantleCosmeticExport::TextureToPng(Texture, Limit, Use != ETextureUse::Colour, Png, Size, Error))
			{
				return false;
			}
			// An upload's normal map is glTF's green-up, which the cook flips once (texturecook.go normalizeNormals).
			// An Unreal texture is green-down unless marked Flip Green Channel, so flip it here, as places do
			// (MantlePlaceAssetConverter.cpp SourceTexturePng). A bake (GLTFExporter) is green-up already.
			if (Use == ETextureUse::Normal && !Texture->bFlipGreenChannel)
			{
				TArray<FColor> Pixels;
				if (!DecodePngPixels(Png, FMath::Max(Size.X, Size.Y), true, Pixels, Size))
				{
					Error = TEXT("its normal map could not be converted to green-up");
					return false;
				}
				for (FColor& Pixel : Pixels)
				{
					Pixel.G = static_cast<uint8>(255 - Pixel.G);
				}
				Png = FMantlePng::EncodeRGBA8(Size.X, Size.Y, Pixels);
			}
			return Png.Num() > 0;
		};
		bool bOk = Encode();
		// The API refuses a texture over MaxCosmeticTextureBytes (backend/internal/api/assets.go). A noisy 2048 normal
		// map is ~11 MiB as PNG, so halve the size until it fits rather than failing the upload halfway through.
		const int32 RequestedLimit = Limit;
		while (bOk && Png.Num() > MaxCosmeticTextureBytes && Limit > 256)
		{
			Limit /= 2;
			bOk = Encode();
		}
		if (!bOk)
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s could not be exported%s."), *What,
				Error.IsEmpty() ? TEXT("") : *(TEXT(": ") + Error))));
			return FString();
		}
		if (Limit < RequestedLimit)
		{
			Package.Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(TEXT("%s is over %d MiB at %dx%d; it is uploaded at %dx%d."),
				*What, static_cast<int32>(MaxCosmeticTextureBytes >> 20), RequestedLimit, RequestedLimit, Size.X, Size.Y)));
		}
		// A texture scaled down to Limit says so in the check list, not only in Files and details.
		FMantleCosmeticExport::CheckSourceTextureSize(Texture, Limit, What, Contract, Package.Issues);
		// The API refuses a grey normal map (backend/internal/cook/texturecook.go scanPixels: every pixel R = G = B). Grey holds
		// no X and Y, so it bends no light: the section goes without one.
		if (Use == ETextureUse::Normal && FMantleCosmeticExport::IsGreyPng(Png))
		{
			Package.Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(TEXT("%s is plain grey, so it holds no surface detail: it is left out."), *What)));
			return FString();
		}
		// The API keeps one role per exact file: a flat mask can match a flat colour map uploaded before, byte for byte, and
		// is then refused (uploaded_differently). A normal or data PNG carries its role
		// in a text chunk, so it never shares bytes with a colour map; colour maps stay plain so they keep matching earlier uploads.
		if (Use != ETextureUse::Colour)
		{
			FMantleCosmeticExport::TagPngRole(Png, Role);
		}
		FMantleAssetUploadOptions Options;
		Options.Kind = TEXT("texture");
		Options.Role = Role;
		Package.AddAsset(Key, FString::Printf(TEXT("%s (%dx%d)"), *What, Size.X, Size.Y), Options, MoveTemp(Png));
		Package.Notes.Add(FString::Printf(TEXT("%s: %s, %dx%d, role %s"), *What, Texture ? *Texture->GetName() : TEXT("baked"), Size.X, Size.Y, Role));
		return Key;
	}

	/**
	 * The section's base colour (its texture, its bake, or its flat colour) with Textures.Opacity packed into the
	 * alpha, as one colour texture, at the base colour's upload size (a flat colour takes the opacity's). The shipped
	 * client draws hair masked with the albedo's alpha as the mask, and the worker keeps that alpha (BC3). The key, or
	 * empty with an error added.
	 */
	FString AddAlbedoWithOpacity(FMantleCosmeticPackage& Package, const FSectionTextures& Textures, int32 MaxSize, const FString& What,
		const FMantleAvatarContract& Contract)
	{
		const FColor Flat = Textures.FlatColour.ToFColorSRGB();
		const FString AlbedoName = Textures.Albedo ? Textures.Albedo->GetPathName()
			: Textures.BakedAlbedo.Num() > 0 ? TEXT("baked.") + What : FString::Printf(TEXT("flat.%02x%02x%02x"), Flat.R, Flat.G, Flat.B);
		const FString Key = FString::Printf(TEXT("texture.%s+opacity.%s.color"), *AlbedoName, *Textures.Opacity->GetPathName());
		if (Package.FindAsset(Key))
		{
			return Key;
		}
		const int32 Limit = FMath::Min(MaxSize > 0 ? MaxSize : Contract.MaxTextureSize, Contract.MaxTextureSize);
		TArray<FColor> Opacity;
		FIntPoint OpacitySize = FIntPoint::ZeroValue;
		FString Error;
		if (!ReadTexturePixels(Textures.Opacity, Limit, true, Opacity, OpacitySize, Error))
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s's opacity could not be exported: %s."), *What, *Error)));
			return FString();
		}
		TArray<FColor> Albedo;
		FIntPoint Size = FIntPoint::ZeroValue;
		if (Textures.Albedo)
		{
			if (!ReadTexturePixels(Textures.Albedo, Limit, false, Albedo, Size, Error))
			{
				Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s could not be exported: %s."), *What, *Error)));
				return FString();
			}
		}
		else if (Textures.BakedAlbedo.Num() > 0)
		{
			if (!DecodePngPixels(Textures.BakedAlbedo, Limit, false, Albedo, Size))
			{
				Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s could not be exported: its bake could not be read."), *What)));
				return FString();
			}
		}
		else
		{
			Size = OpacitySize;
			Albedo.Init(Flat, Size.X * Size.Y);
		}
		FMantleCosmeticExport::CheckSourceTextureSize(Textures.Albedo, Limit, What, Contract, Package.Issues);
		FMantleCosmeticExport::CheckSourceTextureSize(Textures.Opacity, Limit, What + TEXT("'s opacity"), Contract, Package.Issues);
		if (OpacitySize != Size)
		{
			TArray<FColor> Resized;
			Resized.SetNumUninitialized(Size.X * Size.Y);
			FImageUtils::ImageResize(OpacitySize.X, OpacitySize.Y, TArrayView<const FColor>(Opacity), Size.X, Size.Y, TArrayView<FColor>(Resized), false, false);
			Opacity = MoveTemp(Resized);
		}
		const EMantleOpacityChannel Channel = FMantleCosmeticExport::ChooseOpacityChannel(Opacity);
		FMantleCosmeticExport::PackOpacityIntoAlpha(Albedo, Opacity, Channel);
		TArray<uint8> Png = FMantlePng::EncodeRGBA8(Size.X, Size.Y, Albedo);
		if (Png.Num() == 0)
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s with its opacity could not be encoded as PNG."), *What)));
			return FString();
		}
		FMantleAssetUploadOptions Options;
		Options.Kind = TEXT("texture");
		Options.Role = TEXT("color");
		Package.AddAsset(Key, FString::Printf(TEXT("%s with opacity (%dx%d)"), *What, Size.X, Size.Y), Options, MoveTemp(Png));
		Package.Notes.Add(FString::Printf(TEXT("%s: opacity from %s (%s) packed into the base colour's alpha, %dx%d"), *What, *Textures.Opacity->GetName(),
			Channel == EMantleOpacityChannel::Red ? TEXT("its grey or red channel") : TEXT("its alpha"), Size.X, Size.Y));
		return Key;
	}

	/**
	 * One overlay image as BGRA pixels exactly as stored: Texture's source (its stored bytes, whatever its sRGB
	 * flag), else the PNG at File. False with the reason added to Issues (What leads it).
	 */
	bool ReadOverlayImage(UTexture2D* Texture, const FString& File, const FString& What, TArray<FColor>& OutPixels, FIntPoint& OutSize,
		TArray<FMantleCosmeticIssue>& Issues)
	{
		FString Error;
		if (Texture)
		{
			// MaxSize 0: never scaled here, an image over the limit is refused by the checks, not silently shrunk.
			if (!ReadTexturePixels(Texture, 0, !Texture->SRGB, OutPixels, OutSize, Error))
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s could not be read: %s."), *What, *Error)));
				return false;
			}
			return true;
		}
		if (!FMantleOverlayChecks::ReadPngFile(File, OutPixels, OutSize, Error))
		{
			Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s: %s"), *What, *Error)));
			return false;
		}
		return true;
	}

	/**
	 * #54: bakes the placed picture into the overlay's mask (BGRA, FMantleOverlayBake::Size square) and, on a scar slot, its normal map.
	 * The mesh is the draft's placement mesh, else Mantle's reference body or face. False with the reason added to Issues.
	 */
	bool BakePlacedOverlay(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, TArray<FColor>& OutMask, FIntPoint& OutSize, TArray<FColor>& OutNormal,
		TArray<FMantleCosmeticIssue>& Issues)
	{
		TArray<FColor> PicturePixels;
		FIntPoint PictureSize = FIntPoint::ZeroValue;
		if (!ReadOverlayImage(Draft.OverlayPicture.Get(), Draft.OverlayPictureFile, TEXT("The picture"), PicturePixels, PictureSize, Issues))
		{
			return false;
		}
		const TArray<uint8> Strength = FMantleOverlayBake::PictureStrength(PicturePixels, PictureSize.X, PictureSize.Y);
		if (Strength.IsEmpty())
		{
			Issues.Add(FMantleCosmeticChecks::Error(TEXT("the picture to place is empty.")));
			return false;
		}
		USkeletalMesh* Mesh = Draft.OverlayPlaceMesh.Get();
		if (!Mesh)
		{
			// Not picked: the template body or face, else Mantle's reference one.
			Mesh = FMantleOverlayBake::DefaultMesh(FMantleOverlayChecks::IsFaceSlot(Draft.Slot), &Contract);
		}
		if (!Mesh)
		{
			Issues.Add(FMantleCosmeticChecks::Error(TEXT("there is no converted MetaHuman body or face to place the picture on."),
				TEXT("Choose a converted MetaHuman under Preview body (optional) or Preview face (optional) to place it on.")));
			return false;
		}
		TArray<FMantleOverlayBake::FTriangle> Triangles;
		FString Error;
		if (!FMantleOverlayBake::ReadMesh(*Mesh, Triangles, Error))
		{
			Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the picture could not be placed: %s"), *Error)));
			return false;
		}
		TArray<uint8> Mask;
		TArray<FColor> Normal;
		const bool bScar = FMantleOverlayChecks::IsScarSlot(Draft.Slot);
		FMantleOverlayBake::Bake(Triangles, Strength, PictureSize.X, PictureSize.Y, Draft.OverlayPlacement, Mask, bScar ? &Normal : nullptr);
		OutMask = FMantleOverlayBake::MaskToPixels(Mask);
		OutNormal = MoveTemp(Normal);
		OutSize = FIntPoint(FMantleOverlayBake::Size, FMantleOverlayBake::Size);
		return true;
	}

	/**
	 * The package for a scar or tattoo overlay. The mask (and a scar's normal map) are read, checked, and added as
	 * texture assets (the mask as role data, the normal map as role normal); the thumbnail is the mask drawn light on dark. Errors go to Package.Issues.
	 */
	void BuildOverlayPackage(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, FMantleCosmeticPackage& Package)
	{
		TArray<FMantleCosmeticIssue>& Issues = Package.Issues;
		if (!FMantleOverlayChecks::IsOverlaySlot(Draft.Slot))
		{
			Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("\"%s\" is not a slot for an overlay."), *Draft.Slot),
				FString::Printf(TEXT("Choose one of %s."), *FString::Join(FMantleOverlayChecks::Slots(), TEXT(", ")))));
			return;
		}
		UTexture2D* MaskTexture = Cast<UTexture2D>(Draft.Source.Get());
		UTexture2D* NormalTexture = Draft.OverlayNormal.Get();
		bool bHasNormal = NormalTexture || !Draft.OverlayNormalFile.IsEmpty();
		TArray<FColor> MaskPixels;
		FIntPoint MaskSize = FIntPoint::ZeroValue;
		TArray<FColor> NormalPixels;
		FIntPoint NormalSize = FIntPoint::ZeroValue;
		bool bMaskOk = false;
		bool bNormalOk = true;
		if (Draft.bOverlayPlaced)
		{
			// #54: baked from the placed picture, then checked like any other mask.
			if (!BakePlacedOverlay(Draft, Contract, MaskPixels, MaskSize, NormalPixels, Issues))
			{
				return;
			}
			bHasNormal = FMantleOverlayChecks::IsScarSlot(Draft.Slot);
			NormalSize = MaskSize;
			bMaskOk = FMantleOverlayChecks::CheckMask(TEXT("The mask"), MaskSize.X, MaskSize.Y, MaskPixels, Issues);
			bNormalOk = !bHasNormal || FMantleOverlayChecks::CheckNormal(TEXT("The normal map"), NormalSize.X, NormalSize.Y, NormalPixels, Issues);
		}
		else
		{
			if (Draft.Source.IsValid() && !MaskTexture)
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("an overlay's mask is a texture asset or a PNG file."),
					TEXT("Pick a Texture 2D, or clear the asset and choose a PNG file.")));
				return;
			}
			if (!MaskTexture && Draft.OverlayMaskFile.IsEmpty())
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("choose the overlay's mask: a texture asset or a PNG file.")));
				return;
			}
			if (!FMantleOverlayChecks::CheckNormalAllowed(Draft.Slot, bHasNormal, Issues))
			{
				return;
			}

			if (!ReadOverlayImage(MaskTexture, Draft.OverlayMaskFile, TEXT("The mask"), MaskPixels, MaskSize, Issues))
			{
				return;
			}
			bMaskOk = FMantleOverlayChecks::CheckMask(TEXT("The mask"), MaskSize.X, MaskSize.Y, MaskPixels, Issues);
			if (bHasNormal)
			{
				bNormalOk = ReadOverlayImage(NormalTexture, Draft.OverlayNormalFile, TEXT("The normal map"), NormalPixels, NormalSize, Issues)
					&& FMantleOverlayChecks::CheckNormal(TEXT("The normal map"), NormalSize.X, NormalSize.Y, NormalPixels, Issues);
			}
		}
		if (!bMaskOk || !bNormalOk)
		{
			return;
		}

		const int32 Limit = FMantleOverlayChecks::MaxSize;
		const TArray<uint8> MaskPng = FMantlePng::EncodeRGBA8(MaskSize.X, MaskSize.Y, FMantleOverlayChecks::CanonicalMask(MaskPixels));
		Package.MaskKey = AddTexture(Package, nullptr, &MaskPng, ETextureUse::Data, Limit, TEXT("Overlay mask"), Contract);
		if (bHasNormal)
		{
			for (FColor& Pixel : NormalPixels)
			{
				Pixel.A = 255;
			}
			const TArray<uint8> NormalPng = FMantlePng::EncodeRGBA8(NormalSize.X, NormalSize.Y, NormalPixels);
			Package.OverlayNormalKey = AddTexture(Package, nullptr, &NormalPng, ETextureUse::Normal, Limit, TEXT("Overlay normal map"), Contract);
		}
		if (Package.MaskKey.IsEmpty() || (bHasNormal && Package.OverlayNormalKey.IsEmpty()))
		{
			return;
		}

		// #124: the thumbnail is the creator's custom image; else the avatar wearing the mark, by the catalog thumbnail capture from the chosen angle; else, when
		// nothing can render (-NullRHI, a commandlet), the mask itself, light on dark.
		const int32 ThumbSize = Contract.ThumbnailSize;
		const FMantleThumbnailTarget ThumbTarget{ ThumbSize, ThumbSize };
		TArray<uint8> ThumbPng;
		FString ThumbNote;
		const FMantleThumbnailChoice& ThumbChoice = Draft.Thumbnail;
		if (ThumbChoice.Mode == EMantleThumbnailMode::Custom)
		{
			const FMantleThumbnailResult Picture = FMantleThumbnailResolver::Resolve(ThumbChoice, ThumbTarget, FMantleThumbnailResolver::FRender());
			if (!Picture.Succeeded())
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the thumbnail's custom image could not be used: %s"), *Picture.Error),
					TEXT("Choose another picture in the Thumbnail section, or go back to Automatic.")));
				return;
			}
			ThumbPng = FMantleThumbnailResolver::ToPng(Picture, ThumbTarget);
			ThumbNote = Picture.Warning.IsEmpty() ? Picture.Source : FString::Printf(TEXT("%s (%s)"), *Picture.Source, *Picture.Warning);
		}
		else if (FMantleCosmeticThumbnail::CanCapture())
		{
			FMantleOverlayWorn Worn;
			FString Why;
			if (FMantleOverlayWorn::Make(Package, Draft.Slot, Contract, nullptr, nullptr, Worn, Why) && Worn.bDrawn)
			{
				FMantleThumbnailSubject Subject = Worn.ToSubject();
				FMantleModelThumbnail::ApplyChoice(Subject, ThumbChoice);
				TArray<FColor> Pixels;
				FString WhyNot;
				if (FMantleCosmeticThumbnail::Capture(Subject, ThumbSize, Pixels, WhyNot))
				{
					ThumbPng = FMantlePng::EncodeRGBA8(ThumbSize, ThumbSize, Pixels);
					ThumbNote = FString::Printf(TEXT("Thumbnail: scene capture of %s"), *Subject.Describe);
				}
				else
				{
					UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL overlay fallback=mask reason=\"%s\""), *WhyNot);
				}
			}
			else
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL overlay fallback=mask reason=\"%s\""), Why.IsEmpty() ? *Worn.Note : *Why);
			}
		}
		if (ThumbPng.IsEmpty())
		{
			TArray<FColor> Scaled;
			Scaled.SetNumUninitialized(ThumbSize * ThumbSize);
			FImageUtils::ImageResize(MaskSize.X, MaskSize.Y, TArrayView<const FColor>(MaskPixels), ThumbSize, ThumbSize, TArrayView<FColor>(Scaled), false, false);
			for (FColor& Pixel : Scaled)
			{
				const float T = Pixel.R / 255.0f;
				Pixel = FColor(static_cast<uint8>(FMath::Lerp(38.0f, 235.0f, T)), static_cast<uint8>(FMath::Lerp(42.0f, 235.0f, T)),
					static_cast<uint8>(FMath::Lerp(48.0f, 235.0f, T)), 255);
			}
			ThumbPng = FMantlePng::EncodeRGBA8(ThumbSize, ThumbSize, Scaled);
			ThumbNote = TEXT("thumbnail is the mask");
		}
		FMantleAssetUploadOptions ThumbnailOptions;
		ThumbnailOptions.Kind = TEXT("texture");
		Package.AddAsset(TEXT("thumbnail"), TEXT("Thumbnail"), ThumbnailOptions, MoveTemp(ThumbPng));
		Package.ThumbnailKey = TEXT("thumbnail");
		Package.Notes.Add(FString::Printf(TEXT("Overlay %s: mask %dx%d%s; %s"), *Draft.Slot, MaskSize.X, MaskSize.Y,
			bHasNormal ? *FString::Printf(TEXT(", normal map %dx%d"), NormalSize.X, NormalSize.Y) : TEXT(""), *ThumbNote));

		for (const FMantleCosmeticUploadAsset& Asset : Package.Assets)
		{
			FMantleCosmeticChecks::CheckUploadSize(Asset.Label, Asset.Bytes.Num(), Contract, Issues);
		}
	}

	/**
	 * The item's material entries for Source's glTF materials: textures resolved, exported and added. With
	 * bOpacityInAlpha (hair), a section's separate opacity texture is packed into its base colour's alpha.
	 */
	void AddSectionMaterials(FMantleCosmeticPackage& Package, const FMeshSource& Source, bool bNormal, bool bOrm, const FMantleContractClass& Class,
		const TArray<FString>& Roles, const FString& Prefix, TArray<FMantleCosmeticMaterialRef>& OutRefs, const FMantleAvatarContract& Contract,
		bool bOpacityInAlpha = false, bool bAvatarFace = false, bool bNativeEyeOptimization = false)
	{
		for (int32 Index = 0; Index < Source.Materials.Num(); ++Index)
		{
			UMaterialInterface* Material = Source.Materials[Index];
			const FString Section = Source.Glb.MaterialNames.IsValidIndex(Index) ? Source.Glb.MaterialNames[Index] : FString::FromInt(Index);
			const FString Role = Roles.IsValidIndex(Index) ? Roles[Index] : FString();
			const bool bNativeEye = bAvatarFace && Role == TEXT("eyes") && FMantleNativeEyeExport::IsNative(Material);
			if (bNativeEye && bNativeEyeOptimization)
			{
				FMantleNativeEyeSettings Settings; TMap<FName, UTexture2D*> NativeTextures; FString NativeError;
				if (FMantleNativeEyeExport::Read(Material, Settings, NativeTextures, NativeError))
				{
					FMantleCosmeticMaterialRef Ref; Ref.Role = Role; Ref.NativeEye = MoveTemp(Settings);
					for (const TCHAR* Parameter : FMantleNativeEye::TextureParameters())
					{
						const FString TextureRole = FMantleNativeEye::TextureRole(FName(Parameter));
						const ETextureUse Use = TextureRole == TEXT("normal") ? ETextureUse::Normal : TextureRole == TEXT("data") ? ETextureUse::Data : ETextureUse::Colour;
						const int32 ClassLimit = Use == ETextureUse::Normal ? Class.NormalMax : Class.AlbedoMax;
						const int32 Limit = FMantleCosmeticExport::FaceSectionTextureLimit(ClassLimit, Role, Section);
						Ref.NativeEyeTextureKeys.Add(FName(Parameter), AddTexture(Package, NativeTextures.FindChecked(FName(Parameter)), nullptr, Use, Limit,
							FString::Printf(TEXT("Face %s native %s"), *Section, Parameter), Contract));
					}
					// Generic clients keep their established material fields. These alias already
					// exported source textures, so native clients incur no duplicate physical cost.
					Ref.AlbedoKey = Ref.NativeEyeTextureKeys.FindChecked(FName(TEXT("Sclera Basecolor Baked")));
					Ref.NormalKey = Ref.NativeEyeTextureKeys.FindChecked(FName(TEXT("Sclera Normal Baked")));
					Package.Notes.Add(FString::Printf(TEXT("Face %s: native settings and four generated textures exported; four stock iris/sclera textures use the shipped shader directly"), *Section));
					OutRefs.Add(MoveTemp(Ref)); continue;
				}
				Package.Notes.Add(FString::Printf(TEXT("Face %s: native eye profile is unsupported (%s); falling back to a complete material bake to preserve its authored appearance"), *Section, *NativeError));
			}
			FSectionTextures Textures = ResolveSectionTextures(Material, Package.Notes, bOpacityInAlpha);
			if (bNativeEye)
			{
				// A partial sclera texture cannot represent the whole native eye graph. Keep the
				// authored appearance through the ordinary full-material bake when its profile is unsupported.
				Textures = FSectionTextures(); FString BakeNote;
				if (GUsingNullRHI || !BakeMaterialWithGltfExporter(Material, Textures.BakedAlbedo, Textures.BakedNormal, BakeNote)
					|| Textures.BakedAlbedo.IsEmpty())
				{
					Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("Face %s: native eye appearance requires a complete rendered material bake; %s"), *Section,
						GUsingNullRHI ? TEXT("the renderer is unavailable") : *BakeNote)));
					FMantleCosmeticMaterialRef Ref; Ref.Role = Role; OutRefs.Add(Ref); continue;
				}
				Package.Notes.Add(FString::Printf(TEXT("Face %s: full authored eye appearance baked; no native shared iris substitution"), *Section));
			}
			FMantleCosmeticMaterialRef Ref;
			Ref.Role = Roles.IsValidIndex(Index) ? Roles[Index] : FString();
			const int32 AlbedoLimit = bAvatarFace ? FMantleCosmeticExport::FaceSectionTextureLimit(Class.AlbedoMax, Ref.Role, Section) : Class.AlbedoMax;
			const int32 NormalLimit = bAvatarFace ? FMantleCosmeticExport::FaceSectionTextureLimit(Class.NormalMax, Ref.Role, Section) : Class.NormalMax;
			const int32 OrmLimit = bAvatarFace ? FMantleCosmeticExport::FaceSectionTextureLimit(Class.OrmMax, Ref.Role, Section) : Class.OrmMax;
			if (bAvatarFace && (AlbedoLimit != Class.AlbedoMax || NormalLimit != Class.NormalMax || OrmLimit != Class.OrmMax))
			{
				Package.Notes.Add(FString::Printf(TEXT("Face \"%s\": auxiliary eye/teeth textures normalized to at most 1024 pixels to preserve wardrobe memory headroom"), *Section));
			}
			const FString What = FString::Printf(TEXT("%s \"%s\" base colour"), *Prefix, *Section);
			const EBlendMode Blend = Material ? Material->GetBlendMode() : BLEND_Opaque;
			if (bOpacityInAlpha && Blend != BLEND_Opaque && Blend != BLEND_Masked)
			{
				Package.Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(
					TEXT("%s \"%s\": its material is translucent; Mantle draws hair masked (the alpha dithered, then smoothed over frames), so soft edges look a little harder."),
					*Prefix, *Section)));
			}
			const bool bPackOpacity = bOpacityInAlpha && Textures.Opacity != nullptr;
			if (bOpacityInAlpha && !Textures.Opacity && Blend != BLEND_Opaque)
			{
				Package.Notes.Add(FString::Printf(TEXT("%s \"%s\": no separate opacity texture was found, so the base colour's own alpha is the mask"), *Prefix, *Section));
			}
			if (bPackOpacity)
			{
				Ref.AlbedoKey = AddAlbedoWithOpacity(Package, Textures, AlbedoLimit, What, Contract);
			}
			else if (Textures.Albedo || Textures.BakedAlbedo.Num() > 0)
			{
				Ref.AlbedoKey = AddTexture(Package, Textures.Albedo, &Textures.BakedAlbedo, ETextureUse::Colour, AlbedoLimit, What, Contract);
			}
			else
			{
				const FColor Flat = Textures.FlatColour.ToFColorSRGB();
				const FString Key = FString::Printf(TEXT("texture.flat.%02x%02x%02x%02x.color"), Flat.R, Flat.G, Flat.B, Flat.A);
				FMantleAssetUploadOptions Options;
				Options.Kind = TEXT("texture");
				Options.Role = TEXT("color");
				if (!Package.FindAsset(Key))
				{
					Package.AddAsset(Key, FString::Printf(TEXT("Flat colour #%02x%02x%02x"), Flat.R, Flat.G, Flat.B), Options, FMantlePng::EncodeSolid(4, 4, Flat));
				}
				Ref.AlbedoKey = Key;
				Package.Issues.Add(FMantleCosmeticChecks::Warning(
					FString::Printf(TEXT("%s: no base colour texture was found, so it uploads as a flat %s colour."), *What,
						Textures.bFlatColourFound ? TEXT("material") : TEXT("grey")),
					TEXT("Give the material a BaseColor texture parameter if it should look textured.")));
			}
			if (bNormal && (Textures.Normal || Textures.BakedNormal.Num() > 0))
			{
				Ref.NormalKey = AddTexture(Package, Textures.Normal, &Textures.BakedNormal, ETextureUse::Normal, NormalLimit,
					FString::Printf(TEXT("%s \"%s\" normal map"), *Prefix, *Section), Contract);
			}
			if (bOrm && Textures.Orm)
			{
				Ref.OrmKey = AddTexture(Package, Textures.Orm, nullptr, ETextureUse::Data, OrmLimit,
					FString::Printf(TEXT("%s \"%s\" ORM"), *Prefix, *Section), Contract);
			}
			OutRefs.Add(Ref);
		}
	}

	void AddClothingColors(FMantleCosmeticPackage& Package, const FMeshSource& Source,
		TConstArrayView<FMantleClothingColorAuthoring> Declarations, const FMantleContractClass& Class, const FMantleAvatarContract& Contract)
	{
		TSet<FString> Seen;
		TSet<FString> ReplacedAlbedos;
		for (const FMantleClothingColorAuthoring& Declaration : Declarations)
		{
			const int32 MaterialIndex = Source.Glb.MaterialNames.IndexOfByKey(Declaration.MaterialName);
			if (Seen.Contains(Declaration.MaterialName) || !Package.Materials.IsValidIndex(MaterialIndex) || !Declaration.Mask.IsValid() || Declaration.Regions.IsEmpty())
			{ Package.Issues.Add(FMantleCosmeticChecks::Error(TEXT("Each color declaration needs a unique exported clothing material, a mask and one to three regions."))); continue; }
			Seen.Add(Declaration.MaterialName);
			UTexture2D* MaskTexture = Declaration.Mask.Get();
			if (MaskTexture->SRGB || MaskTexture->Source.GetNumBlocks() != 1)
			{ Package.Issues.Add(FMantleCosmeticChecks::Error(TEXT("A clothing color mask must be a linear data texture with one UV tile."), TEXT("Disable sRGB on the explicitly authored RGB mask."))); continue; }
			FMantleCosmeticMaterialRef& Ref = Package.Materials[MaterialIndex];
			const FString What = TEXT("Clothing ") + Declaration.MaterialName;
			if (Declaration.NeutralAlbedo.IsValid())
			{
				ReplacedAlbedos.Add(Ref.AlbedoKey);
				Ref.AlbedoKey = AddTexture(Package, Declaration.NeutralAlbedo.Get(), nullptr, ETextureUse::Colour, Class.AlbedoMax,
					What + TEXT(" explicit neutral albedo"), Contract);
				Package.Notes.Add(What + TEXT(": the creator explicitly replaced its albedo with a neutral authored texture for dyeing."));
			}
			const FString MaskKey = AddTexture(Package, MaskTexture, nullptr, ETextureUse::Data, Class.AlbedoMax, What + TEXT(" color mask"), Contract);
			const FMantleCosmeticUploadAsset* AlbedoAsset = Package.FindAsset(Ref.AlbedoKey);
			const FMantleCosmeticUploadAsset* MaskAsset = Package.FindAsset(MaskKey);
			TArray<FColor> AlbedoPixels, MaskPixels; FIntPoint AlbedoSize, MaskSize;
			if (!AlbedoAsset || !MaskAsset || !DecodePngPixels(AlbedoAsset->Bytes, Class.AlbedoMax, false, AlbedoPixels, AlbedoSize) ||
				!DecodePngPixels(MaskAsset->Bytes, Class.AlbedoMax, true, MaskPixels, MaskSize))
			{ Package.Issues.Add(FMantleCosmeticChecks::Error(What + TEXT(": its exported albedo or mask could not be measured."))); continue; }
			if (MaskSize != AlbedoSize)
			{
				TArray<FColor> Resized; Resized.SetNumUninitialized(AlbedoSize.X * AlbedoSize.Y);
				FImageUtils::ImageResize(MaskSize.X, MaskSize.Y, TArrayView<const FColor>(MaskPixels), AlbedoSize.X, AlbedoSize.Y, TArrayView<FColor>(Resized), false, false);
				MaskPixels = MoveTemp(Resized);
			}
			TArray<FMantleClothingColorRegion> Regions = Declaration.Regions; FString WhyNot;
			if (!FMantleClothingColorMeasure::Measure(Source.Glb, MaterialIndex, AlbedoSize, AlbedoPixels, MaskPixels, Regions, WhyNot))
			{ Package.Issues.Add(FMantleCosmeticChecks::Error(What + TEXT(": ") + WhyNot)); continue; }
			Ref.ColorMaskKey = MaskKey; Ref.ColorRegions = MoveTemp(Regions);
			for (const FMantleClothingColorRegion& Region : Ref.ColorRegions)
				Package.Notes.Add(FString::Printf(TEXT("%s: declared %s on %s, measured linear reference luminance %.8f over its UV0 mask."), *What, *Region.Id,
					Region.Channel == 0 ? TEXT("R") : Region.Channel == 1 ? TEXT("G") : TEXT("B"), Region.ReferenceLuminance));
		}
		for (const FString& OldAlbedo : ReplacedAlbedos)
		{
			if (!Package.Materials.ContainsByPredicate([&OldAlbedo](const FMantleCosmeticMaterialRef& Ref)
				{ return Ref.AlbedoKey == OldAlbedo || Ref.NormalKey == OldAlbedo || Ref.OrmKey == OldAlbedo || Ref.ColorMaskKey == OldAlbedo; }))
				Package.Assets.RemoveAll([&OldAlbedo](const FMantleCosmeticUploadAsset& Asset) { return Asset.Key == OldAlbedo; });
		}
	}

	/** The first LOD within MaxTriangles, else LOD 0. Only the sections of KeepSlots count, when it is given. */
	int32 ChooseLODWithin(const USkeletalMesh* Mesh, int32 MaxTriangles, const TArray<int32>* KeepSlots = nullptr)
	{
		const int32 NumLODs = UMantleBodyTopologyData::GetNumLODs(Mesh);
		for (int32 LOD = 0; LOD < NumLODs; ++LOD)
		{
			if (MaxTriangles <= 0 || FMantleCosmeticExport::CountTriangles(Mesh, LOD, KeepSlots) <= MaxTriangles)
			{
				return LOD;
			}
		}
		return 0;
	}

	/** The body a garment or hair is authored on, exported as its body shape (with warp weights). */
	struct FBodyShapeResult
	{
		TMap<FName, FTransform> ComponentPose;
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		float KneeZ = TNumericLimits<float>::Max();
		bool bOk = false;
	};

	/**
	 * The body shape upload. With Face (the surface of the face the body was authored with, as it uploads), the
	 * shape also carries the neck measured on it and its own warp weights (FMantleCatalogGarmentFit::ComputeNeck); a
	 * neck that cannot be measured is a warning, and the shape uploads without it (the collar is then fitted from the
	 * body alone, as before).
	 */
	FBodyShapeResult AddBodyShape(FMantleCosmeticPackage& Package, USkeletalMesh* Body, const FMantleSkinnedMeshLOD* Face, const FMantleAvatarContract& Contract)
	{
		FBodyShapeResult Result;
		const int32 LOD = FMantleCosmeticExport::FindReferenceLOD(Body, Contract);
		if (LOD == INDEX_NONE)
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(
				FString::Printf(TEXT("the body %s has no LOD with Mantle's reference topology (%d vertices), so it cannot be the body this was authored on."),
					*Body->GetName(), Contract.TopologyVertexCount),
				TEXT("Choose the MetaHuman body you made it on, exported with UE Optimized at Medium, High or Cinematic quality, without clothing (a Cinematic MetaHuman from Fab: its SKM_<Name>_Body under CharacterAssemblies/<Name>/Body).")));
			return Result;
		}
		FMeshSource Source;
		FString Error;
		if (!ReadSkeletalLOD(Body, LOD, Source, Error))
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(Error));
			return Result;
		}
		if (Source.Glb.Positions.Num() > Contract.TopologyVertexCount)
		{
			const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::Get();
			if (!Layout || !CanonicalizeNativeBodyLOD(Source, *Layout, Error))
			{ Package.Issues.Add(FMantleCosmeticChecks::Error(Error)); return Result; }
			Package.Notes.Add(MantleCosmeticExportPrivate::MergedBodySplitsNote(Source.MergedBodySplits));
		}
		const FMantleContractSkeleton* MetaHuman = Contract.FindSkeleton(TEXT("metahuman"));
		TArray<FMantleSkinJointName> Names;
		int32 Dropped = 0;
		FitSkinToFixture(Source, *MetaHuman, Names, Dropped);
		if (!FMantleCosmeticChecks::MatchSkeleton(Names, *MetaHuman, true, Error))
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_SKELETON the body %s"), *Body->GetName());
			Package.Issues.Add(FMantleCosmeticChecks::Error(FMantleCosmeticChecks::UnsupportedSkeleton(Error)));
			return Result;
		}

		FMantleBodyShape Shape;
		Shape.Positions = Source.Glb.Positions;
		Shape.ReferencePose = FMantleReferencePose::FromReferenceSkeleton(Body->GetRefSkeleton());
		Shape.PositionsHash = UMantleBodyTopologyData::ComputePositionsHash(Shape.Positions);
		double Ms = 0.0;
		if (!FMantleGarmentFitter::ComputeSourceWeights(Shape, Contract.WarpPointCount, Contract.TopologyVertexCount, &Error, &Ms))
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the body shape's warp weights could not be computed: %s"), *Error)));
			return Result;
		}
		Source.Glb.WarpSampleIndices.Reserve(Shape.Weights.SampleIndices.Num());
		for (const int32 Sample : Shape.Weights.SampleIndices)
		{
			Source.Glb.WarpSampleIndices.Add(static_cast<uint32>(Sample));
		}
		Source.Glb.WarpInterpolationWeights = Shape.Weights.InterpolationWeights;

		// The neck, measured on the face the body was authored with.
		if (Face)
		{
			FMantleBodyShapeNeck Neck;
			double NeckMs = 0.0;
			FString NeckError;
			if (FMantleCatalogGarmentFit::ComputeNeck(Shape, *Face, Contract.WarpPointCount, Neck, &NeckError, &NeckMs))
			{
				Source.Glb.NeckMethod = Neck.Method == EMantleNeckMethod::Correspondence ? TEXT("correspondence") : TEXT("rays");
				for (const int32 Id : Neck.Ids)
				{
					Source.Glb.NeckIds.Add(static_cast<uint32>(Id));
				}
				Source.Glb.NeckPositions = Neck.Positions;
				for (const int32 Sample : Neck.Weights.SampleIndices)
				{
					Source.Glb.NeckSampleIndices.Add(static_cast<uint32>(Sample));
				}
				Source.Glb.NeckInterpolationWeights = Neck.Weights.InterpolationWeights;
				Package.Notes.Add(FString::Printf(TEXT("Neck: %d samples on the face by %s, %d warp samples (%.0f ms)"),
					Neck.Positions.Num(), *Source.Glb.NeckMethod, Neck.Weights.SampleIndices.Num(), NeckMs));
			}
			else
			{
				Package.Issues.Add(FMantleCosmeticChecks::Warning(
					FString::Printf(TEXT("the neck could not be measured on the face (%s), so collars are fitted from the body alone."), *NeckError),
					TEXT("It can still be published; collars may sit off the neck of much thinner or heavier wearers.")));
			}
		}

		TArray<uint8> Glb;
		if (!FMantleGlbWriter::Write(Source.Glb, Glb, Error))
		{
			Package.Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the body shape could not be written: %s"), *Error)));
			return Result;
		}
		FMantleAssetUploadOptions Options;
		Options.Kind = TEXT("bodyshape");
		Package.AddAsset(TEXT("shape"), FString::Printf(TEXT("Body shape (%s LOD %d)"), *Body->GetName(), LOD), Options, MoveTemp(Glb));
		Package.SourceShapeKey = TEXT("shape");
		Package.Notes.Add(FString::Printf(TEXT("Body shape: %s LOD %d, %d vertices, %d warp samples (%.0f ms)"),
			*Body->GetName(), LOD, Shape.Positions.Num(), Shape.Weights.SampleIndices.Num(), Ms));

		const TArray<FMeshBoneInfo>& BoneInfo = Body->GetRefSkeleton().GetRawRefBoneInfo();
		for (int32 Bone = 0; Bone < BoneInfo.Num() && Bone < Source.ComponentPose.Num(); ++Bone)
		{
			Result.ComponentPose.Add(BoneInfo[Bone].Name, Source.ComponentPose[Bone]);
			if (Contract.KneeBones.Contains(BoneInfo[Bone].Name))
			{
				Result.KneeZ = FMath::Min(Result.KneeZ, static_cast<float>(Source.ComponentPose[Bone].GetTranslation().Z));
			}
		}
		Result.Positions = MoveTemp(Source.Glb.Positions);
		Result.Normals = MoveTemp(Source.Glb.Normals);
		Result.bOk = true;
		return Result;
	}

	/**
	 * A MetaHuman face read for upload (an avatar's face, and the face a garment was authored with, the same
	 * way, so the same face is the same bytes and uploads once): its reference face topology LOD, else the first
	 * within the face class's triangle limit, fitted to the face skeleton. False with the sentence for the creator.
	 */
	bool ReadFaceSource(const USkeletalMesh* Face, const FMantleAvatarContract& Contract, FMeshSource& Out, FString& OutError,
		bool bNormalizeHeadSource = false, bool* OutGenerated = nullptr, FString* OutNote = nullptr)
	{
		if (OutGenerated) *OutGenerated = false;
		const int32 NumFaceLODs = UMantleBodyTopologyData::GetNumLODs(Face);
		TArray<int32> Counts;
		for (int32 Candidate = 0; Candidate < NumFaceLODs; ++Candidate) Counts.Add(FMantleCosmeticExport::CountTriangles(Face,Candidate));
		int32 FaceLOD = FMantleCosmeticExport::ChooseHeadSourceLOD(Counts,Contract.AvatarFace.MaxTriangles);
		if (FaceLOD == INDEX_NONE && bNormalizeHeadSource)
		{
			// Preserve the verified authoring head's surface, UV charts and rig. The
			// existing reducer owns a transient duplicate and never changes Face.
			int32 SourceLOD = INDEX_NONE;
			for (int32 LOD = 0; LOD < Counts.Num(); ++LOD)
				if (Counts[LOD] > 0 && (SourceLOD == INDEX_NONE || Counts[LOD] < Counts[SourceLOD])) SourceLOD = LOD;
			if (SourceLOD == INDEX_NONE || Contract.AvatarFace.MaxTriangles <= 0)
			{ OutError = TEXT("the authoring head has no valid native surface to normalize"); return false; }
			const float Fraction = 0.85f * Contract.AvatarFace.MaxTriangles / Counts[SourceLOD];
			TArray<FMeshSource> Reduced;
			if (!GenerateReducedLODs(Face,SourceLOD,TArray<float>{Fraction},Reduced,OutError)) return false;
			if (Reduced.Num() != 1 || Reduced[0].NumTriangles <= 0 || Reduced[0].NumTriangles > Contract.AvatarFace.MaxTriangles)
			{ OutError = TEXT("the same-source head reduction did not meet the face triangle limit"); return false; }
			Out = MoveTemp(Reduced[0]);
			Out.LODIndex = SourceLOD; // Provenance only: the generated LOD belongs to the transient duplicate.
			if (OutGenerated) *OutGenerated = true;
		}
		else if (!ReadSkeletalLOD(Face,FaceLOD == INDEX_NONE ? 0 : FaceLOD,Out,OutError))
		{
			return false;
		}
		if (FMantleFitBodies::IsCreatorPreview(Face))
		{
			// A face made in memory by MetaHuman Creator numbers its triangles differently from Epic's built faces, and the client pairs a collar's
			// samples by triangle index: put it in the built face layout (FMantleFaceLayout), or leave it out (the caller warns) rather than tear collars.
			const FMantleFaceLayout* Layout = FMantleFaceLayout::Get();
			FString Note;
			if (!Layout)
			{
				OutError = TEXT("MetaHuman Creator's face is numbered differently from Epic's built faces and the built face layout (Resources/FaceLayout) is missing; Mantle.Dev.RecordFaceLayout records it");
				return false;
			}
			FString Why;
			float BaseZ = 0.0f;
			const bool bHasBase = FMantleFitBodies::NeckBaseZ(Face, BaseZ);
			if (Layout->IsNumberedLikeThis(Out.Glb, Why, bHasBase ? &BaseZ : nullptr, &Note))
			{
				// The normal case: the fit bodies' faces are made from the DNA archetype like the build's, so they number the collar's triangles like built faces.
				if (OutNote) { *OutNote = Note; }
			}
			else if (!Layout->Reorder(Out.Glb, Note, OutError))
			{
				// Fallback only: a face that is numbered differently and does not match the built layout is left out rather than tear collars.
				OutError = TEXT("MetaHuman Creator's face is not numbered like Epic's built faces (") + Why + TEXT(") and does not match the built face layout: ") + OutError;
				return false;
			}
			else if (OutNote) { *OutNote = Note; }
		}
		int32 Dropped = 0;
		if (!FitToClassSkeleton(Out, Contract.AvatarFace, Contract, true, Dropped, OutError))
		{
			OutError = FMantleCosmeticChecks::EndWithFullStop(TEXT("the face: ") + OutError);
			return false;
		}
		return true;
	}

	/**
	 * The surface of a face as it uploads (LOD0 of its GLB: its vertices, and its primitives' triangles in order,
	 * which is the cooked MSK1's order): what the neck and the collar's authored face clearance are measured on, here
	 * and on players' clients.
	 */
	FMantleSkinnedMeshLOD UploadedFaceSurface(const FMeshSource& Face)
	{
		FMantleSkinnedMeshLOD Out;
		Out.Positions = Face.Glb.Positions;
		Out.Normals = Face.Glb.Normals;
		for (const FMantleGlbPrimitive& Primitive : Face.Glb.Primitives)
		{
			for (const uint32 Index : Primitive.Indices)
			{
				Out.Indices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
			}
		}
		Out.Sections.Add({ 0, 0, Out.Indices.Num() });
		return Out;
	}

	FMantleSkinnedMesh PreviewMesh(const FMeshSource& Source, const TArray<FMeshSource>& Lower = {})
	{
		FMantleSkinnedMesh Out;
		for (const auto& Joint : Source.Glb.Joints) { Out.Bones.Add({Joint.Name, Joint.Parent}); Out.ReferencePose.Add(Joint.Local); }
		auto Add = [&Out](const FMeshSource& Mesh)
		{
			const auto& G = Mesh.Glb; auto& L = Out.LODs.AddDefaulted_GetRef();
			L.Positions = G.Positions; L.Normals = G.Normals; L.Tangents = G.Tangents; L.UVs = G.UVs; L.NumInfluences = G.NumInfluences;
			L.InfluenceBones = G.InfluenceJoints; L.InfluenceWeights.SetNumZeroed(G.InfluenceWeights.Num());
			for (int32 V = 0; V < G.Positions.Num(); ++V)
			{
				int32 Sum = 0, Largest = 0;
				for (int32 K = 0; K < G.NumInfluences; ++K) { const int32 I = V * G.NumInfluences + K; L.InfluenceWeights[I] = static_cast<uint8>(FMath::FloorToInt(255.f * G.InfluenceWeights[I])); Sum += L.InfluenceWeights[I]; if (G.InfluenceWeights[I] > G.InfluenceWeights[V * G.NumInfluences + Largest]) Largest = K; }
				if (G.NumInfluences > 0) L.InfluenceWeights[V * G.NumInfluences + Largest] += static_cast<uint8>(255 - Sum);
				if (!G.HairCoordU.IsEmpty()) L.HairCards.Add(FVector3f(G.HairRootUV[V].X, G.HairRootUV[V].Y, G.HairCoordU[V]));
			}
			for (const auto& P : G.Primitives) { L.Sections.Add({P.MaterialIndex, L.Indices.Num(), P.Indices.Num(), FName(*G.MaterialNames[P.MaterialIndex])}); for (uint32 I : P.Indices) L.Indices.Add(I + P.FirstVertex); }
		};
		Add(Source); for (const auto& L : Lower) Add(L); return Out;
	}

	/** The "face" upload (an avatar asset's face part) with its generated LODs, named as the item's face mesh. False with the reason. */
	bool AddFaceAsset(FMantleCosmeticPackage& Package, const USkeletalMesh* Face, const FMeshSource& FaceMesh, const bool bGenerateLODs,
		const FMantleAvatarContract& Contract, const FString& Label, FString& OutError, const bool bGeometryOnly = false)
	{
		const TArray<FMeshSource> FaceLODs = bGenerateLODs ? MakeLowerLODs(Face, FaceMesh, Contract, TEXT("Face"), Package) : TArray<FMeshSource>();
		TArray<uint8> FaceGlb;
		if (!WriteMeshWithLODs(FaceMesh, FaceLODs, FaceGlb, OutError, bGeometryOnly))
		{
			return false;
		}
		FMantleAssetUploadOptions FaceOptions;
		FaceOptions.Kind = TEXT("avatar");
		FaceOptions.Part = TEXT("face");
		Package.AddAsset(TEXT("face"), Label, FaceOptions, MoveTemp(FaceGlb));
		Package.FaceMeshKey = TEXT("face");
		return true;
	}

	/**
	 * The cook names each cooked section after its GLB material, and a converted upload's sections
	 * must carry their client material ids (skin_body_v1, ...). Renames LOD0's and every lower LOD's
	 * sections from the source slot names to the ids BuildClientAppearanceMaterials resolved.
	 */
	void NameSectionsByClientMaterial(FMeshSource& Mesh, TArray<FMeshSource>& Lower, const TArray<FMantleCosmeticMaterialRef>& Materials)
	{
		TMap<FString, FString> ClientNames;
		for (int32 Index = 0; Index < Mesh.Glb.MaterialNames.Num() && Index < Materials.Num(); ++Index)
		{
			ClientNames.Add(Mesh.Glb.MaterialNames[Index], Materials[Index].ClientMaterial);
		}
		auto Rename = [&ClientNames](FMantleGlbMesh& Glb)
		{
			for (FString& Name : Glb.MaterialNames)
			{
				if (const FString* Client = ClientNames.Find(Name)) Name = *Client;
			}
		};
		Rename(Mesh.Glb);
		for (FMeshSource& LOD : Lower) Rename(LOD.Glb);
	}

	/** A 512 x 512 thumbnail of Object from the editor's thumbnail renderer, or a plain one when nothing can render. */
	TArray<uint8> RenderThumbnail(UObject* Object, int32 Size, bool bRender, FString& OutNote)
	{
		if (bRender && Object && !GUsingNullRHI)
		{
			FObjectThumbnail Thumbnail;
			ThumbnailTools::RenderThumbnail(Object, Size, Size, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
			const TArray<uint8>& Data = Thumbnail.GetUncompressedImageData();
			if (Thumbnail.GetImageWidth() == Size && Thumbnail.GetImageHeight() == Size && Data.Num() == Size * Size * 4)
			{
				TArray<FColor> Pixels;
				Pixels.SetNumUninitialized(Size * Size);
				for (int32 Pixel = 0; Pixel < Size * Size; ++Pixel)
				{
					// The renderer's BGRA; its alpha is not coverage, so the thumbnail is opaque.
					Pixels[Pixel] = FColor(Data[Pixel * 4 + 2], Data[Pixel * 4 + 1], Data[Pixel * 4], 255);
				}
				OutNote = FString::Printf(TEXT("Thumbnail rendered from %s"), *Object->GetName());
				return FMantlePng::EncodeRGBA8(Size, Size, Pixels);
			}
		}
		OutNote = TEXT("Thumbnail: a plain one (nothing could render here)");
		return FMantlePng::EncodeSolid(Size, Size, FColor(38, 42, 48, 255));
	}

	/**
	 * The item as players see it, by scene capture (FMantleCosmeticThumbnail): an avatar's
	 * headshot with its face, a garment on the body it was authored on, an accessory alone. Hair and
	 * emotes, and a process that cannot render (-NullRHI, the headless tests), take the
	 * thumbnail renderer's path, which itself falls back to a plain image; the log says which. A garment
	 * with KeepSlots shows only those sections, framed on GarmentFrame (the kept vertices) when it is valid.
	 */
	TArray<uint8> MakeThumbnail(const FMantleCosmeticDraft& Draft, UObject* Source, USkeletalMesh* AuthoringBody, int32 Size, FString& OutNote,
		FString& OutProblem, const FBox& GarmentFrame = FBox(ForceInit), TConstArrayView<uint8> EmoteClip = TConstArrayView<uint8>())
	{
		const FMantleThumbnailChoice& Choice = Draft.Thumbnail;
		if (Choice.Mode == EMantleThumbnailMode::Custom)
		{
			// #124: the creator's own picture, cut square and resized; needs no renderer.
			const FMantleThumbnailTarget Target{ Size, Size };
			const FMantleThumbnailResult Picture = FMantleThumbnailResolver::Resolve(Choice, Target, FMantleThumbnailResolver::FRender());
			if (!Picture.Succeeded())
			{
				OutProblem = Picture.Error;
				return TArray<uint8>();
			}
			OutNote = Picture.Warning.IsEmpty() ? Picture.Source : FString::Printf(TEXT("%s (%s)"), *Picture.Source, *Picture.Warning);
			return FMantleThumbnailResolver::ToPng(Picture, Target);
		}
		if (!Draft.bRenderThumbnail)
		{
			return RenderThumbnail(Source, Size, false, OutNote);
		}
		FMantleThumbnailSubject Subject;
		switch (Draft.Class)
		{
		case EMantleCosmeticClass::Avatar:
			Subject = FMantleCosmeticThumbnail::AvatarSubject(Cast<USkeletalMesh>(Source), Draft.Face.Get());
			break;
		case EMantleCosmeticClass::Garment:
			Subject = FMantleCosmeticThumbnail::GarmentSubject(Cast<USkeletalMesh>(Source), AuthoringBody,
				FMantleCosmeticExport::FindAuthoringFace(Draft, AuthoringBody), Draft.KeepSlots.GetPtrOrNull(), GarmentFrame);
			break;
		case EMantleCosmeticClass::Accessory:
			Subject = FMantleCosmeticThumbnail::AccessorySubject(Cast<UStaticMesh>(Source), Draft.AccessoryWorn, Draft.bAccessoryBackFaces);
			break;
		case EMantleCosmeticClass::Emote:
			// #124: Mantle's standard body posed as players see it, at the emote's most expressive moment.
			if (!EmoteClip.IsEmpty() && FMantleCosmeticThumbnail::CanCapture())
			{
				FString How;
				FString WhyNot;
				if (FMantleEmoteCheck::ThumbnailSubject(EmoteClip, Subject, How, WhyNot))
				{
					UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL emote %s"), *How);
				}
				else
				{
					UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL emote fallback=thumbnail-renderer reason=\"%s\""), *WhyNot);
				}
			}
			break;
		default:
			break;
		}
		// #124: the same picture from the angle the creator chose.
		FMantleModelThumbnail::ApplyChoice(Subject, Choice);
		if (!Subject.IsEmpty())
		{
			FString WhyNot;
			TArray<FColor> Pixels;
			if (FMantleCosmeticThumbnail::Capture(Subject, Size, Pixels, WhyNot))
			{
				OutNote = FString::Printf(TEXT("Thumbnail: scene capture of %s"), *Subject.Describe);
				UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL capture %s"), *Subject.Describe);
				return FMantlePng::EncodeRGBA8(Size, Size, Pixels);
			}
			UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_THUMBNAIL fallback=thumbnail-renderer subject=\"%s\" reason=\"%s\""), *Subject.Describe, *WhyNot);
		}
		return RenderThumbnail(Source, Size, true, OutNote);
	}

	/**
	 * What sampling an emote changed or left out (FMantleAnimClipData's RootMotion, DroppedNotifies, DroppedCurves and
	 * LoopSeam*), as notes, and as warnings for what the creator will see differently on players' screens.
	 */
	void AddEmoteSamplingNotes(const FMantleAnimClipData& Clip, const FMantleAvatarContract& Contract, FMantleCosmeticPackage& Package,
		TArray<FMantleCosmeticIssue>& Issues)
	{
		const FMantleAnimClipData::FRootMotion& Root = Clip.RootMotion;
		if (Root.bInPlace)
		{
			Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(
				TEXT("the animation travels %.0f cm across the floor; emotes play in place (at most %g cm), so it plays on the spot and its feet may slide."),
				Root.TravelCm, Contract.EmoteMaxRootCm),
				TEXT("For a cleaner result, remove the travel from the root in the source animation and upload again.")));
		}
		if (Root.bMoved)
		{
			Package.Notes.Add(FString::Printf(TEXT("Emote root motion moved onto the pelvis, as players' clients hold the root still: %.1f cm across the floor%s, %.1f cm up and down, %.1f degrees of turn, starting %.1f cm and %.1f degrees off the root's reference"),
				Root.TravelCm, Root.bInPlace ? TEXT(" (taken out: in place)") : TEXT(""), Root.RiseCm, Root.TurnDegrees, Root.OffsetCm, Root.OffsetDegrees));
		}
		if (Clip.DroppedNotifies.Num() > 0)
		{
			Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("%d notify name(s) will not play: %s. Emotes carry only the events %s; sounds and effects on notifies are left out."),
				Clip.DroppedNotifies.Num(), *FString::Join(Clip.DroppedNotifies, TEXT(", ")), *FString::Join(Contract.EmoteEvents, TEXT(", ")))));
		}
		if (Clip.DroppedCurves > 0)
		{
			Package.Notes.Add(FString::Printf(TEXT("Emote: %d animation curve(s) left out (emotes carry bone motion only)"), Clip.DroppedCurves));
		}
		if (Clip.LoopSeamDegrees > 0.5 || Clip.LoopSeamCm > 0.5)
		{
			Package.Notes.Add(FString::Printf(TEXT("Emote loop: the end was %.1f degrees and %.1f cm from the loop_start pose, and now blends into it over its last frames, so the loop does not jump"),
				Clip.LoopSeamDegrees, Clip.LoopSeamCm));
		}
	}

	/** Length of the common prefix of two names, case-insensitive. */
	int32 CommonPrefix(const FString& A, const FString& B)
	{
		int32 Length = 0;
		while (Length < A.Len() && Length < B.Len() && FChar::ToLower(A[Length]) == FChar::ToLower(B[Length]))
		{
			++Length;
		}
		return Length;
	}

	/**
	 * A cards static mesh on its own (a groom's cards migrated without the groom, as in projects
	 * without the HairStrands plugin, or one of MetaHuman's per-groom CardsMesh assets): its own
	 * atlases, the attribute atlas (coverage, depth, seed) first and the tangent atlas second, never
	 * another groom's. MetaHuman keeps every groom's cards mesh in one Grooms folder but the eyebrow
	 * and moustache atlases under Common/Optional/Grooms/GroomAssets, tied to the cards only by the
	 * groom asset; the head hair's attribute atlas sits under that Grooms folder, so a nearest-name
	 * search gave the eyebrows the hair's atlas.
	 *
	 * Candidates, strongest link first: textures the mesh's own slot materials reference (engine
	 * materials aside); textures the groom named for the mesh's stem references (an asset of class
	 * GroomAsset under the mesh's folder or its parent); textures under the mesh's folder or its
	 * parent, which count only when named for the stem. FMantleCosmeticExport::PickCardAtlases
	 * chooses. Empty when no attribute atlas belongs to the mesh; OutHow says, per texture, what
	 * tied it to the mesh.
	 */
	TArray<UTexture2D*> FindCardAtlasesNear(const UStaticMesh* Cards, TArray<FString>& OutHow)
	{
		TArray<UTexture2D*> Out;
		OutHow.Reset();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FString Folder = FPackageName::GetLongPackagePath(Cards->GetOutermost()->GetName());
		const FString Parent = FPackageName::GetLongPackagePath(Folder);
		const FString MeshName = Cards->GetName();
		const FString Stem = FMantleCosmeticExport::CardsGroomStem(MeshName);
		const FTopLevelAssetPath TextureClass = UTexture2D::StaticClass()->GetClassPathName();

		TArray<FMantleCardAtlasCandidate> Candidates;
		TArray<FAssetData> CandidateAssets;
		TArray<FString> LinkedBy;
		auto AddCandidate = [&](const FAssetData& Data, EMantleCardAtlasLink Link, const FString& By)
		{
			if (Data.AssetClassPath != TextureClass)
			{
				return;
			}
			const FSoftObjectPath Path = Data.GetSoftObjectPath();
			const int32 Existing = CandidateAssets.IndexOfByPredicate([&Path](const FAssetData& Other) { return Other.GetSoftObjectPath() == Path; });
			if (Existing != INDEX_NONE)
			{
				if (Link < Candidates[Existing].Link)
				{
					Candidates[Existing].Link = Link;
					LinkedBy[Existing] = By;
				}
				return;
			}
			FMantleCardAtlasCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Name = Data.AssetName.ToString();
			Candidate.PackagePath = Data.PackagePath.ToString();
			Candidate.Link = Link;
			CandidateAssets.Add(Data);
			LinkedBy.Add(By);
		};
		auto AddReferencedTextures = [&](const FName PackageName, EMantleCardAtlasLink Link, const FString& By)
		{
			TArray<FName> Dependencies;
			Registry.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
			for (const FName Dependency : Dependencies)
			{
				TArray<FAssetData> Assets;
				Registry.GetAssetsByPackageName(Dependency, Assets);
				for (const FAssetData& Data : Assets)
				{
					AddCandidate(Data, Link, By);
				}
			}
		};

		for (const FStaticMaterial& Slot : Cards->GetStaticMaterials())
		{
			const UMaterialInterface* Material = Slot.MaterialInterface;
			const FString Package = Material ? Material->GetOutermost()->GetName() : FString();
			if (Package.IsEmpty() || Package.StartsWith(TEXT("/Engine/")) || Package.StartsWith(TEXT("/Script/")))
			{
				continue;
			}
			AddReferencedTextures(FName(*Package), EMantleCardAtlasLink::Material, FString::Printf(TEXT("its material %s"), *Material->GetName()));
		}

		TArray<FAssetData> Nearby;
		Registry.GetAssetsByPath(FName(*(Parent.IsEmpty() ? Folder : Parent)), Nearby, true);
		for (const FAssetData& Data : Nearby)
		{
			if (Data.AssetName.ToString().Equals(Stem, ESearchCase::IgnoreCase) && Data.AssetClassPath.GetAssetName() == FName(TEXT("GroomAsset")))
			{
				AddReferencedTextures(Data.PackageName, EMantleCardAtlasLink::Groom, FString::Printf(TEXT("its groom %s"), *Data.PackageName.ToString()));
			}
		}
		for (const FAssetData& Data : Nearby)
		{
			AddCandidate(Data, EMantleCardAtlasLink::Name, FString::Printf(TEXT("its name (groom %s)"), *Stem));
		}

		int32 Attribute = INDEX_NONE;
		int32 Tangent = INDEX_NONE;
		FMantleCosmeticExport::PickCardAtlases(MeshName, Folder, Candidates, Attribute, Tangent);
		UTexture2D* AttributeTexture = Attribute != INDEX_NONE ? Cast<UTexture2D>(CandidateAssets[Attribute].GetAsset()) : nullptr;
		if (!AttributeTexture)
		{
			return Out;
		}
		Out.Add(AttributeTexture);
		OutHow.Add(LinkedBy[Attribute]);
		if (UTexture2D* TangentTexture = Tangent != INDEX_NONE ? Cast<UTexture2D>(CandidateAssets[Tangent].GetAsset()) : nullptr)
		{
			Out.Add(TangentTexture);
			OutHow.Add(LinkedBy[Tangent]);
		}
		return Out;
	}
	FString DefaultRole(const FString& Slot)
	{
		const FString Name = Slot.ToLower();
		if (Name.Contains(TEXT("eyelash")) || Name.Contains(TEXT("eyeshell")) || Name.Contains(TEXT("eyeedge")) || Name.Contains(TEXT("teeth"))
			|| Name.Contains(TEXT("saliva")) || Name.Contains(TEXT("cartilage")))
		{
			return TEXT("other");
		}
		if (Name.Contains(TEXT("eyeleft")) || Name.Contains(TEXT("eyeright")) || Name.Contains(TEXT("eye_l")) || Name.Contains(TEXT("eye_r"))
			|| Name.Contains(TEXT("eyeball")))
		{
			return TEXT("eyes");
		}
		if (Name.Contains(TEXT("head")) || Name.Contains(TEXT("body")) || Name.Contains(TEXT("skin")))
		{
			return TEXT("skin");
		}
		return TEXT("other");
	}

	/**
	 * The one decision for whether a hair source gets the upload-time head fit (FMantleHairPieceFit, then
	 * FMantleScalpFit). Bought hair is made for a different head; MetaHuman's own grooms and anything generated from them
	 * already fit and are never touched. The decision is made from the source itself: only a standalone
	 * cards static mesh qualifies, and only when the editor finds no MetaHuman provenance for it (no recorded original
	 * groom or authoring head, no groom that references the cards, no groom binding: FMantleHairSourceDiscovery's
	 * HasAssociation). Grooms and skeletal hair are always OFF.
	 */
	bool ShouldFitThirdPartyHairCardsToHead(const UObject* Source)
	{
		if (!Source || !Source->IsA<UStaticMesh>()) return false;
		FString Ignored;
		bool bHasMetaHumanAssociation = false;
		FMantleHairSourceDiscovery::FindHead(Source, Ignored, &bHasMetaHumanAssociation);
		return !bHasMetaHumanAssociation;
	}

	/**
	 * Sizes and seats a bought cards pack on the authoring head (stage 1, the whole-piece fit), then settles it on
	 * the scalp (stage 2, the smoothed offset field and the last-resort lift). Positions only; topology, UVs and normals are
	 * untouched. A failure leaves the cards as they were and says so in a warning, it never blocks the upload. The pack's
	 * solid cap, if it has one, is found in the mesh and split from the cards (FMantleHairCardsFit).
	 */
	void FitThirdPartyHairCardsToHead(FMeshSource& Cards, const FMeshSource& Face, FMantleCosmeticPackage& Package)
	{
		TArray<uint32> SkinIndices, AllFaceIndices;
		for (const FMantleGlbPrimitive& Primitive : Face.Glb.Primitives)
		{
			const FString Name = Face.Glb.MaterialNames.IsValidIndex(Primitive.MaterialIndex) ? Face.Glb.MaterialNames[Primitive.MaterialIndex] : FString();
			const bool bSkin = DefaultRole(Name).Equals(TEXT("skin"), ESearchCase::CaseSensitive);
			for (const uint32 Index : Primitive.Indices)
			{
				AllFaceIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
				if (bSkin) SkinIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
			}
		}
		// A head with no section named as skin: the whole face mesh is the surface.
		const TArray<uint32>& HeadIndices = SkinIndices.IsEmpty() ? AllFaceIndices : SkinIndices;

		// The hair's triangles, each with its section, so the cap can be told from the cards and a baked-in head from both.
		TArray<uint32> HairIndices;
		TArray<int32> TriangleSections;
		for (const FMantleGlbPrimitive& Primitive : Cards.Glb.Primitives)
		{
			for (const uint32 Index : Primitive.Indices) HairIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
			for (int32 T = 0; T < Primitive.Indices.Num() / 3; ++T) TriangleSections.Add(Primitive.MaterialIndex);
		}

		FMantleHairCardsFit::FInput Input;
		Input.HeadPositions = Face.Glb.Positions;
		Input.HeadIndices = HeadIndices;
		Input.HairPositions = Cards.Glb.Positions;
		Input.HairIndices = HairIndices;
		Input.TriangleSections = TriangleSections;
		Input.SectionNames = Cards.Glb.MaterialNames;
		const FMantleHairCardsFit::FResult Fit = FMantleHairCardsFit::Run(Input);
		for (const FString& Warning : Fit.Warnings) Package.Issues.Add(FMantleCosmeticChecks::Warning(Warning));
		Package.Notes.Append(Fit.Notes);
		if (!Fit.bFitted) return;
		// The hair as authored, turned upright when the file was Y-up (the fit's positions are turned too).
		Package.HairHeadFitMeshes.BeforePositions = Cards.Glb.Positions;
		if (Fit.QuarterTurns != 0)
		{
			for (FVector3f& P : Package.HairHeadFitMeshes.BeforePositions) P = FMantleHairCardsFit::RotateUpright(P, Fit.QuarterTurns);
		}
		Package.HairHeadFitMeshes.AfterPositions = Fit.Positions;
		Package.HairHeadFitMeshes.HairTriangles = HairIndices;
		Package.HairHeadFitMeshes.HeadPositions = Face.Glb.Positions;
		Package.HairHeadFitMeshes.HeadTriangles = HeadIndices;
		Cards.Glb.Positions = Fit.Positions;
		// Hair turned upright (a Y-up OBJ): its shading directions turn with it.
		if (Fit.QuarterTurns != 0)
		{
			for (FVector3f& N : Cards.Glb.Normals) N = FMantleHairCardsFit::RotateUpright(N, Fit.QuarterTurns);
			for (FVector4f& T : Cards.Glb.Tangents)
			{
				const FVector3f Turned = FMantleHairCardsFit::RotateUpright(FVector3f(T.X, T.Y, T.Z), Fit.QuarterTurns);
				T = FVector4f(Turned.X, Turned.Y, Turned.Z, T.W);
			}
		}
		Package.HairHeadFit = Fit.Report;
		Package.bHairHeadFitted = true;
		Cards.Bounds = FBox3f(ForceInit);
		for (const FVector3f& P : Cards.Glb.Positions) Cards.Bounds += P;
	}

	bool ResolveClientAppearanceMaterial(const FString& MaterialName, const FString& Role, const bool bFace,
		FString& OutClientMaterial, FString& OutRole, FString& OutError)
	{
		const FString Name = MaterialName.ToLower();
		OutClientMaterial.Reset();
		OutRole.Reset();
		if (!bFace)
		{
			if (Role == TEXT("skin") && (Name.Contains(TEXT("body")) || Name.Contains(TEXT("skin"))))
			{
				OutClientMaterial = TEXT("skin_body_v1");
				OutRole = TEXT("skin");
				return true;
			}
			OutError = FString::Printf(TEXT("Body section \"%s\" is not the supported MetaHuman skin slot."), *MaterialName);
			return false;
		}

		if (Role == TEXT("skin") && (Name.Contains(TEXT("head")) || Name.Contains(TEXT("face")) || Name.Contains(TEXT("skin")))
			&& !Name.Contains(TEXT("eye")) && !Name.Contains(TEXT("teeth")))
		{
			OutClientMaterial = TEXT("skin_face_v1");
			OutRole = TEXT("skin");
			return true;
		}
		if (Role == TEXT("eyes"))
		{
			const bool bLeft = Name.Contains(TEXT("eyeleft")) || Name.Contains(TEXT("eye_left")) || Name.Contains(TEXT("left_eye")) || Name.Contains(TEXT("eye_l_"));
			const bool bRight = Name.Contains(TEXT("eyeright")) || Name.Contains(TEXT("eye_right")) || Name.Contains(TEXT("right_eye")) || Name.Contains(TEXT("eye_r_"));
			if (bLeft != bRight)
			{
				OutClientMaterial = bLeft ? TEXT("eye_left_v1") : TEXT("eye_right_v1");
				OutRole = TEXT("eyes");
				return true;
			}
		}
		if (Role == TEXT("other"))
		{
			if (Name.Contains(TEXT("eyelash"))) OutClientMaterial = TEXT("eyelashes_v1");
			else if (Name.Contains(TEXT("eyeshell")) || Name.Contains(TEXT("eye_shell"))) OutClientMaterial = TEXT("eye_shell_v1");
			else if (Name.Contains(TEXT("eyeedge")) || Name.Contains(TEXT("eye_edge")) || Name.Contains(TEXT("eyeocclusion")) || Name.Contains(TEXT("eye_occlusion"))) OutClientMaterial = TEXT("eye_occlusion_v1");
			else if (Name.Contains(TEXT("teeth"))) OutClientMaterial = TEXT("teeth_v1");
			if (!OutClientMaterial.IsEmpty())
			{
				OutRole = TEXT("other");
				return true;
			}
		}
		OutError = FString::Printf(TEXT("Face section \"%s\" does not map to a supported client appearance slot and role."), *MaterialName);
		return false;
	}

	bool HasClientAppearanceUVProfile(const FMantleGlbMesh& Mesh, const int32 MaterialIndex,
		const FString& ClientMaterial, const TCHAR* SectionRole, FString& OutError)
	{
		int32 SetCount = 0;
		uint32 SetHash = 0;
		int32 TriangleCount = 0;
		uint32 TriangleHash = 0;
		uint32 AlternateTriangleHash = 0;
		if (ClientMaterial == TEXT("skin_face_v1"))
		{
			SetCount = ClientFaceSkinUVSetCount; SetHash = ClientFaceSkinUVSetHash;
			TriangleCount = ClientFaceSkinUVTriangleCount; TriangleHash = ClientFaceSkinUVTriangleHash;
			AlternateTriangleHash = 0x2b130cc9u;
		}
		else if (ClientMaterial == TEXT("eye_left_v1") || ClientMaterial == TEXT("eye_right_v1"))
		{
			SetCount = ClientEyeUVSetCount; SetHash = ClientEyeUVSetHash;
			TriangleCount = ClientEyeUVTriangleCount; TriangleHash = ClientEyeUVTriangleHash;
		}
		else if (ClientMaterial == TEXT("teeth_v1"))
		{
			SetCount = 523; SetHash = 0x0041fedd; TriangleCount = 796; TriangleHash = 0x34c9b26b;
		}
		else if (ClientMaterial == TEXT("eye_shell_v1"))
		{
			SetCount = 60; SetHash = 0x44b3a8a1; TriangleCount = 196; TriangleHash = 0x487df82f;
			AlternateTriangleHash = TriangleHash; // any triangulation of its UV set (below)
		}
		else if (ClientMaterial == TEXT("eyelashes_v1"))
		{
			SetCount = 374; SetHash = 0x3db972cb; TriangleCount = 250; TriangleHash = 0x057a0143;
		}
		else if (ClientMaterial == TEXT("eye_occlusion_v1"))
		{
			SetCount = 118; SetHash = 0xb8e07242; TriangleCount = 150; TriangleHash = 0x895e5891;
			AlternateTriangleHash = TriangleHash; // any triangulation of its UV set (below)
		}
		else
		{
			OutError = FString::Printf(TEXT("The converted face %s section has no verified client UV0 profile for %s."), SectionRole, *ClientMaterial);
			return false;
		}

		if (!HasClientAppearanceUVSet(Mesh, MaterialIndex, SetCount, SetHash, SectionRole, OutError)) return false;
		if (AlternateTriangleHash != 0)
		{
			TArray<FQuantizedUVTriangle> Triangles;
			for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
			{
				if (Primitive.MaterialIndex == MaterialIndex && !AppendQuantizedUVTriangles(Mesh, Primitive, Triangles, OutError))
				{
					OutError = FString::Printf(TEXT("The converted face %s section has invalid triangle UV0 evidence: %s"), SectionRole, *OutError);
					return false;
				}
			}
			const uint32 ActualHash = HashQuantizedUVTriangleMultiset(Triangles);
			// Any triangulation of the verified skin UV set (the API and client accept it too):
			// Epic's face builds split the same quads differently per character.
			const bool bAnyTriangulation = ClientMaterial == TEXT("skin_face_v1") || ClientMaterial == TEXT("eye_shell_v1") || ClientMaterial == TEXT("eye_occlusion_v1");
			if (Triangles.Num() != TriangleCount || (!bAnyTriangulation && ActualHash != TriangleHash && ActualHash != AlternateTriangleHash))
			{
				OutError = FString::Printf(TEXT("The converted face %s section does not match either verified UV0 triangle profile (got %d / %08x)."),
					SectionRole, Triangles.Num(), ActualHash);
				return false;
			}
			return true;
		}
		return HasClientAppearanceUVTriangleProfile(Mesh, MaterialIndex, TriangleCount, TriangleHash, SectionRole, OutError);
	}

	bool HasClientEyeSideWeights(const FMantleGlbMesh& Mesh, const int32 MaterialIndex, const bool bLeft, FString& OutError)
	{
		const int32 VertexCount = Mesh.Positions.Num();
		if (Mesh.NumInfluences < 1 || Mesh.InfluenceJoints.Num() != VertexCount * Mesh.NumInfluences ||
			Mesh.InfluenceWeights.Num() != VertexCount * Mesh.NumInfluences)
		{
			OutError = TEXT("The face eye section has incomplete decoded skinning data.");
			return false;
		}
		TSet<int32> Vertices;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			if (Primitive.MaterialIndex != MaterialIndex) continue;
			if (Primitive.FirstVertex < 0 || Primitive.NumVertices <= 0 || Primitive.FirstVertex + Primitive.NumVertices > VertexCount)
			{
				OutError = TEXT("A face eye primitive has an invalid vertex range.");
				return false;
			}
			for (const uint32 Index : Primitive.Indices)
			{
				if (Index >= static_cast<uint32>(Primitive.NumVertices))
				{
					OutError = TEXT("A face eye primitive has an out-of-range triangle index.");
					return false;
				}
				Vertices.Add(Primitive.FirstVertex + static_cast<int32>(Index));
			}
		}
		if (Vertices.IsEmpty())
		{
			OutError = TEXT("A required face eye material has no indexed geometry.");
			return false;
		}
		double SideWeight = 0.0;
		double TotalWeight = 0.0;
		for (const int32 Vertex : Vertices)
		{
			for (int32 Influence = 0; Influence < Mesh.NumInfluences; ++Influence)
			{
				const int32 Index = Vertex * Mesh.NumInfluences + Influence;
				const float Weight = Mesh.InfluenceWeights[Index];
				const uint16 JointIndex = Mesh.InfluenceJoints[Index];
				if (!FMath::IsFinite(Weight) || Weight < 0.0f || !Mesh.Joints.IsValidIndex(JointIndex))
				{
					OutError = TEXT("A face eye vertex has an invalid joint or influence weight.");
					return false;
				}
				TotalWeight += Weight;
				const FString BoneName = Mesh.Joints[JointIndex].Name.ToString();
				if (IsEyeParallelBone(BoneName, bLeft))
				{
					SideWeight += Weight;
				}
			}
		}
		if (TotalWeight <= 0.0 || SideWeight / TotalWeight < 0.80)
		{
			OutError = FString::Printf(TEXT("The %s eye section is less than 80%% weighted to the matching MetaHuman eye bones."), bLeft ? TEXT("left") : TEXT("right"));
			return false;
		}
		return true;
	}
}

FString FMantleGroomCardsEntry::Describe() const
{
	return FString::Printf(TEXT("Group %d, LOD %d: %s (%d textures)"), GroupIndex, LODIndex,
		Mesh.IsValid() ? *Mesh->GetName() : TEXT("no mesh"), Textures.Num());
}

bool FMantleCosmeticExport::IsGroomAsset(const UObject* Object)
{
	for (const UClass* Class = Object ? Object->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
	{
		if (Class->GetFName() == FName(TEXT("GroomAsset")))
		{
			return true;
		}
	}
	return false;
}

TArray<FMantleGroomCardsEntry> FMantleCosmeticExport::ReadGroomCards(const UObject* Groom)
{
	TArray<FMantleGroomCardsEntry> Out;
	if (!IsGroomAsset(Groom))
	{
		return Out;
	}
	// Reflection, so the plugin needs no HairStrands module (UGroomAsset::HairGroupsCards,
	// FHairGroupsCardsSourceDescription: ImportedMesh, GroupIndex, LODIndex, Textures.Textures).
	const FArrayProperty* Cards = FindFProperty<FArrayProperty>(Groom->GetClass(), TEXT("HairGroupsCards"));
	const FStructProperty* Inner = Cards ? CastField<FStructProperty>(Cards->Inner) : nullptr;
	if (!Inner)
	{
		return Out;
	}
	const UScriptStruct* Entry = Inner->Struct;
	const FObjectPropertyBase* MeshProperty = FindFProperty<FObjectPropertyBase>(Entry, TEXT("ImportedMesh"));
	const FIntProperty* LODProperty = FindFProperty<FIntProperty>(Entry, TEXT("LODIndex"));
	const FIntProperty* GroupProperty = FindFProperty<FIntProperty>(Entry, TEXT("GroupIndex"));
	const FStructProperty* TexturesProperty = FindFProperty<FStructProperty>(Entry, TEXT("Textures"));
	const FArrayProperty* TextureList = TexturesProperty ? FindFProperty<FArrayProperty>(TexturesProperty->Struct, TEXT("Textures")) : nullptr;
	const FObjectPropertyBase* TextureItem = TextureList ? CastField<FObjectPropertyBase>(TextureList->Inner) : nullptr;
	const FProperty* LayoutProperty = TexturesProperty ? FindFProperty<FProperty>(TexturesProperty->Struct, TEXT("Layout")) : nullptr;
	const FBoolProperty* InvertUVProperty = FindFProperty<FBoolProperty>(Entry, TEXT("bInvertUV"));
	const FNameProperty* EntrySlotProperty = FindFProperty<FNameProperty>(Entry, TEXT("MaterialSlotName"));

	// The groom's materials (UGroomAsset::HairGroupsMaterials, FHairGroupsMaterial: Material, SlotName), where its hair
	// colour lives.
	TArray<TPair<FName, UMaterialInterface*>> GroomMaterials;
	if (const FArrayProperty* MaterialList = FindFProperty<FArrayProperty>(Groom->GetClass(), TEXT("HairGroupsMaterials")))
	{
		const FStructProperty* MaterialStruct = CastField<FStructProperty>(MaterialList->Inner);
		const FObjectPropertyBase* MaterialProperty = MaterialStruct ? FindFProperty<FObjectPropertyBase>(MaterialStruct->Struct, TEXT("Material")) : nullptr;
		const FNameProperty* SlotProperty = MaterialStruct ? FindFProperty<FNameProperty>(MaterialStruct->Struct, TEXT("SlotName")) : nullptr;
		if (MaterialProperty)
		{
			FScriptArrayHelper Materials(MaterialList, MaterialList->ContainerPtrToValuePtr<void>(Groom));
			for (int32 Index = 0; Index < Materials.Num(); ++Index)
			{
				const uint8* Element = Materials.GetRawPtr(Index);
				if (UMaterialInterface* Material = Cast<UMaterialInterface>(MaterialProperty->GetObjectPropertyValue_InContainer(Element)))
				{
					GroomMaterials.Emplace(SlotProperty ? SlotProperty->GetPropertyValue_InContainer(Element) : NAME_None, Material);
				}
			}
		}
	}

	FScriptArrayHelper Array(Cards, Cards->ContainerPtrToValuePtr<void>(Groom));
	for (int32 Index = 0; Index < Array.Num(); ++Index)
	{
		const uint8* Element = Array.GetRawPtr(Index);
		FMantleGroomCardsEntry& Card = Out.AddDefaulted_GetRef();
		Card.Index = Index;
		Card.bInvertUV = InvertUVProperty && InvertUVProperty->GetPropertyValue_InContainer(Element);
		if (MeshProperty)
		{
			Card.Mesh = Cast<UStaticMesh>(MeshProperty->GetObjectPropertyValue_InContainer(Element));
		}
		if (LODProperty)
		{
			Card.LODIndex = LODProperty->GetPropertyValue_InContainer(Element);
		}
		if (GroupProperty)
		{
			Card.GroupIndex = GroupProperty->GetPropertyValue_InContainer(Element);
		}
		if (TexturesProperty && TextureList && TextureItem)
		{
			const void* TexturesValue = TexturesProperty->ContainerPtrToValuePtr<void>(Element);
			const FNumericProperty* NumericLayout = CastField<FNumericProperty>(LayoutProperty);
			if (const FEnumProperty* Enum = CastField<FEnumProperty>(LayoutProperty)) NumericLayout = Enum->GetUnderlyingProperty();
			if (NumericLayout) Card.TextureLayout = static_cast<int32>(NumericLayout->GetSignedIntPropertyValue(LayoutProperty->ContainerPtrToValuePtr<void>(TexturesValue)));
			FScriptArrayHelper Textures(TextureList, TextureList->ContainerPtrToValuePtr<void>(TexturesValue));
			for (int32 Texture = 0; Texture < Textures.Num(); ++Texture)
			{
				// Null slots matter: the shader's texture layout indexes the full array.
				Card.Textures.Add(Cast<UTexture2D>(TextureItem->GetObjectPropertyValue(Textures.GetRawPtr(Texture))));
			}
		}
		FName Slot = EntrySlotProperty ? EntrySlotProperty->GetPropertyValue_InContainer(Element) : NAME_None;
		if (Slot.IsNone())
		{
			if (const FArrayProperty* Rendering = FindFProperty<FArrayProperty>(Groom->GetClass(), TEXT("HairGroupsRendering")))
			{
				const FStructProperty* GroupStruct = CastField<FStructProperty>(Rendering->Inner);
				const FNameProperty* GroupSlot = GroupStruct ? FindFProperty<FNameProperty>(GroupStruct->Struct, TEXT("MaterialSlotName")) : nullptr;
				FScriptArrayHelper Groups(Rendering, Rendering->ContainerPtrToValuePtr<void>(Groom));
				if (GroupSlot && Card.GroupIndex >= 0 && Card.GroupIndex < Groups.Num()) Slot = GroupSlot->GetPropertyValue_InContainer(Groups.GetRawPtr(Card.GroupIndex));
			}
		}
		for (const TPair<FName, UMaterialInterface*>& Material : GroomMaterials)
		{
			if (!Slot.IsNone() && Material.Key == Slot)
			{
				Card.Materials.Insert(TWeakObjectPtr<UMaterialInterface>(Material.Value), 0);
				Card.EffectiveCardsMaterial = Material.Value;
			}
			else
			{
				Card.Materials.Add(TWeakObjectPtr<UMaterialInterface>(Material.Value));
			}
		}
		if (Slot.IsNone() && GroomMaterials.Num() == 1) Card.EffectiveCardsMaterial = GroomMaterials[0].Value;
	}
	return Out;
}

int32 FMantleCosmeticExport::DefaultCardsEntry(const TArray<FMantleGroomCardsEntry>& Entries)
{
	int32 Best = INDEX_NONE;
	for (const FMantleGroomCardsEntry& Entry : Entries)
	{
		if (!Entry.Mesh.IsValid())
		{
			continue;
		}
		if (Entry.LODIndex == 3)
		{
			return Entry.Index;
		}
		if (Best == INDEX_NONE || Entry.LODIndex < Entries[Best].LODIndex)
		{
			Best = Entry.Index;
		}
	}
	return Best;
}

FString FMantleCosmeticExport::CardsGroomStem(const FString& CardsMeshName)
{
	FString Stem = CardsMeshName;
	if (Stem.StartsWith(TEXT("SM_"), ESearchCase::IgnoreCase))
	{
		Stem.RightChopInline(3);
	}
	const int32 CardsAt = Stem.Find(TEXT("_CardsMesh"), ESearchCase::IgnoreCase);
	if (CardsAt > 0)
	{
		return Stem.Left(CardsAt);
	}
	auto DropNumbered = [&Stem](const TCHAR* Word)
	{
		const int32 At = Stem.Find(Word, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (At <= 0)
		{
			return;
		}
		const FString Tail = Stem.Mid(At + FCString::Strlen(Word));
		bool bDigits = !Tail.IsEmpty();
		for (const TCHAR Char : Tail)
		{
			bDigits = bDigits && FChar::IsDigit(Char);
		}
		if (bDigits)
		{
			Stem.LeftInline(At);
		}
	};
	DropNumbered(TEXT("_LOD"));
	DropNumbered(TEXT("_Group"));
	return Stem.IsEmpty() ? CardsMeshName : Stem;
}

bool FMantleCosmeticExport::CardAtlasBelongsTo(const FString& TextureName, const FString& Stem)
{
	if (Stem.IsEmpty())
	{
		return false;
	}
	FString Name = TextureName;
	if (!Name.StartsWith(Stem, ESearchCase::IgnoreCase) && Name.StartsWith(TEXT("T_"), ESearchCase::IgnoreCase))
	{
		Name.RightChopInline(2);
	}
	if (!Name.StartsWith(Stem, ESearchCase::IgnoreCase) || Name.Len() <= Stem.Len() + 1 || Name[Stem.Len()] != TEXT('_'))
	{
		return false;
	}
	// The atlas word straight after the stem: "Hair_S_Casual_Long_CardsAtlas_Attribute" is another groom's.
	const FString Rest = Name.Mid(Stem.Len() + 1);
	static const TCHAR* const AtlasWords[] = { TEXT("Cards"), TEXT("Atlas"), TEXT("Attribute"), TEXT("Coverage"), TEXT("Tangent") };
	for (const TCHAR* Word : AtlasWords)
	{
		if (Rest.StartsWith(Word, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

void FMantleCosmeticExport::PickCardAtlases(const FString& CardsMeshName, const FString& MeshFolder, const TArray<FMantleCardAtlasCandidate>& Candidates,
	int32& OutAttribute, int32& OutTangent)
{
	using namespace MantleCosmeticExportPrivate;

	OutAttribute = INDEX_NONE;
	OutTangent = INDEX_NONE;
	const FString Stem = CardsGroomStem(CardsMeshName);
	int32* Out[2] = { &OutAttribute, &OutTangent };
	int32 BestScore[2] = { -1, -1 };
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FMantleCardAtlasCandidate& Candidate = Candidates[Index];
		const int32 Kind = NameMatches(Candidate.Name, { TEXT("Tangent") }) ? 1
			: NameMatches(Candidate.Name, { TEXT("Attribute"), TEXT("Coverage") }) ? 0 : INDEX_NONE;
		if (Kind == INDEX_NONE || !NameMatches(Candidate.Name, { TEXT("Atlas"), TEXT("Cards") }))
		{
			continue;
		}
		const bool bNamed = CardAtlasBelongsTo(Candidate.Name, Stem);
		if (Candidate.Link == EMantleCardAtlasLink::Name && !bNamed)
		{
			// Near the mesh but named for another groom (the head hair's atlas beside the eyebrows' cards).
			continue;
		}
		const int32 LinkScore = Candidate.Link == EMantleCardAtlasLink::Material ? 2 : Candidate.Link == EMantleCardAtlasLink::Groom ? 1 : 0;
		const int32 Score = LinkScore * 100000 + (bNamed ? 10000 : 0) + (Candidate.PackagePath.Equals(MeshFolder, ESearchCase::IgnoreCase) ? 1000 : 0)
			+ FMath::Min(CommonPrefix(Candidate.Name, CardsMeshName), 999);
		if (Score > BestScore[Kind])
		{
			BestScore[Kind] = Score;
			*Out[Kind] = Index;
		}
	}
}

int32 FMantleCosmeticExport::ChooseHeadSourceLOD(TConstArrayView<int32> TriangleCounts, int32 MaxTriangles)
{
	int32 First = INDEX_NONE;
	for (int32 LOD = 0; LOD < TriangleCounts.Num(); ++LOD)
	{
		const int32 Count = TriangleCounts[LOD];
		if (Count <= 0 || (MaxTriangles > 0 && Count > MaxTriangles)) continue;
		if (Count == MantleCosmeticExportPrivate::ReferenceFaceTriangles) return LOD;
		if (First == INDEX_NONE) First = LOD;
	}
	return First;
}

int32 FMantleCosmeticExport::ChooseGroomCardsLOD(const TArray<TArray<int32>>& PerLODGroupTriangles, int32 MaxTriangles)
{
	// A LOD slightly over budget, trimmed down to it, looks nearly identical to the full hair, while a much coarser LOD
	// looks visibly different. Beyond this margin the trim removes enough whole cards to leave bald patches.
	constexpr int64 TrimMarginPercent = 15;
	int32 Coarsest = INDEX_NONE;
	for (int32 LOD = 0; LOD < PerLODGroupTriangles.Num(); ++LOD)
	{
		int64 Total = 0;
		for (const int32 Count : PerLODGroupTriangles[LOD]) Total += FMath::Max(Count, 0);
		if (Total <= 0) continue;
		if (MaxTriangles <= 0 || Total * 100 <= static_cast<int64>(MaxTriangles) * (100 + TrimMarginPercent)) return LOD;
		Coarsest = LOD;
	}
	return Coarsest;
}

int32 FMantleCosmeticExport::CountTriangles(const USkeletalMesh* Mesh, int32 LODIndex, const TArray<int32>* KeepSlots)
{
	return CountKeptTriangles(ReadSectionSpans(Mesh, LODIndex), KeepSlots);
}

TArray<FMantleSectionSpan> FMantleCosmeticExport::ReadSectionSpans(const USkeletalMesh* Mesh, int32 LODIndex)
{
	TArray<FMantleSectionSpan> Out;
	const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return Out;
	}
	const TArray<FSkelMeshRenderSection>& Sections = RenderData->LODRenderData[LODIndex].RenderSections;
	for (int32 Section = 0; Section < Sections.Num(); ++Section)
	{
		FMantleSectionSpan& Span = Out.AddDefaulted_GetRef();
		Span.Slot = MantleCosmeticExportPrivate::SectionSlot(Mesh, LODIndex, Section, Sections[Section].MaterialIndex);
		Span.FirstVertex = static_cast<int32>(Sections[Section].BaseVertexIndex);
		Span.NumVertices = static_cast<int32>(Sections[Section].NumVertices);
		Span.NumTriangles = static_cast<int32>(Sections[Section].NumTriangles);
	}
	return Out;
}

TArray<int32> FMantleCosmeticExport::KeptSections(TConstArrayView<FMantleSectionSpan> Sections, const TArray<int32>* KeepSlots)
{
	TArray<int32> Out;
	for (int32 Section = 0; Section < Sections.Num(); ++Section)
	{
		if (!KeepSlots || KeepSlots->Contains(Sections[Section].Slot))
		{
			Out.Add(Section);
		}
	}
	Out.StableSort([Sections](int32 A, int32 B) { return Sections[A].FirstVertex < Sections[B].FirstVertex; });
	return Out;
}

int32 FMantleCosmeticExport::CountKeptTriangles(TConstArrayView<FMantleSectionSpan> Sections, const TArray<int32>* KeepSlots)
{
	int32 Triangles = 0;
	for (const int32 Section : KeptSections(Sections, KeepSlots))
	{
		Triangles += Sections[Section].NumTriangles;
	}
	return Triangles;
}

TArray<FMantleOutfitSection> FMantleCosmeticExport::ReadOutfitSections(const USkeletalMesh* Mesh, int32 LODIndex, TArray<int32>& OutSlots)
{
	TArray<FMantleOutfitSection> Out;
	OutSlots.Reset();
	if (!Mesh)
	{
		return Out;
	}
	const TArray<FMantleSectionSpan> Spans = ReadSectionSpans(Mesh, LODIndex);
	const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
	for (const int32 Section : KeptSections(Spans, nullptr))
	{
		const int32 Slot = Spans[Section].Slot;
		if (OutSlots.Contains(Slot))
		{
			continue;
		}
		OutSlots.Add(Slot);
		UMaterialInterface* Material = Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialInterface.Get() : nullptr;
		const UMaterial* BaseMaterial = Material ? Material->GetBaseMaterial() : nullptr;
		FMantleOutfitSection& Entry = Out.AddDefaulted_GetRef();
		Entry.SlotName = MantleCosmeticExportPrivate::SlotName(Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName : NAME_None, Material, Slot);
		Entry.MaterialName = Material ? Material->GetName() : FString();
		Entry.BaseMaterialName = BaseMaterial ? BaseMaterial->GetName() : FString();
	}
	return Out;
}

int32 FMantleCosmeticExport::AvatarBodyLOD(const USkeletalMesh* Body, const FMantleAvatarContract& Contract)
{
	const int32 ReferenceLOD = FindReferenceLOD(Body, Contract);
	return ReferenceLOD != INDEX_NONE ? ReferenceLOD : MantleCosmeticExportPrivate::ChooseLODWithin(Body, Contract.AvatarBody.MaxTriangles);
}

bool FMantleCosmeticExport::AvatarSkeletonId(const USkeletalMesh* Body, const FMantleAvatarContract& Contract, FString& OutSkeletonId, FString& OutError)
{
	using namespace MantleCosmeticExportPrivate;
	if (!Body)
	{
		OutError = TEXT("Choose the avatar's body skeletal mesh.");
		return false;
	}
	// Build's avatar path: the LOD it uploads, fitted to the avatar class's skeletons.
	FMeshSource Mesh;
	if (!ReadSkeletalLOD(Body, AvatarBodyLOD(Body, Contract), Mesh, OutError))
	{
		return false;
	}
	int32 Dropped = 0;
	const FMantleContractSkeleton* Skeleton = FitToClassSkeleton(Mesh, Contract.AvatarBody, Contract, true, Dropped, OutError);
	if (!Skeleton)
	{
		return false;
	}
	OutSkeletonId = Skeleton->Id;
	return true;
}

TArray<FMantleCosmeticSocketOverride> FMantleCosmeticExport::DefaultSocketOverrides(const USkeletalMesh* Body, const FMantleAvatarContract& Contract)
{
	TArray<FMantleCosmeticSocketOverride> Out;
	// The accessory sockets, then limits.json's toolSockets (hand_rSocket, on hand_r in every skeleton fixture).
	TArray<TPair<FString, FString>> Sockets;
	for (const FString& Socket : Contract.AccessorySockets())
	{
		const FMantleContractSlot* Slot = Contract.Slots.FindByPredicate([&Socket](const FMantleContractSlot& Entry) { return Entry.Socket == Socket; });
		Sockets.Emplace(Socket, Slot ? Slot->SocketBone : FString());
	}
	for (const FMantleContractToolSocket& Tool : Contract.ToolSockets)
	{
		Sockets.Emplace(Tool.Socket, Tool.SocketBone);
	}
	for (const TPair<FString, FString>& Entry : Sockets)
	{
		const FString& Socket = Entry.Key;
		FMantleCosmeticSocketOverride& Override = Out.AddDefaulted_GetRef();
		Override.Socket = Socket;
		Override.Bone = Entry.Value;
		// The body's own socket (MetaHuman and SK_Mannequin skeletons carry Mantle's),
		// when it hangs off the bone the contract names: its values are relative to that bone.
		const USkeletalMeshSocket* Own = Body ? Body->FindSocket(FName(*Socket)) : nullptr;
		if (Own && (Override.Bone.IsEmpty() || Own->BoneName == FName(*Override.Bone)))
		{
			Override.Location = FVector3f(Own->RelativeLocation);
			Override.Rotation = FVector3f(static_cast<float>(Own->RelativeRotation.Pitch), static_cast<float>(Own->RelativeRotation.Yaw),
				static_cast<float>(Own->RelativeRotation.Roll));
			Override.Scale = FVector3f(Own->RelativeScale);
		}
	}
	return Out;
}

int32 FMantleCosmeticExport::FindReferenceLOD(const USkeletalMesh* Mesh, const FMantleAvatarContract& Contract)
{
	const int32 NumLODs = UMantleBodyTopologyData::GetNumLODs(Mesh);
	for (int32 LOD = 0; LOD < NumLODs; ++LOD)
	{
		if (HasReferenceTopology(Mesh, LOD, Contract))
		{
			return LOD;
		}
	}
	return INDEX_NONE;
}

namespace MantleCosmeticExportPrivate
{
	/** A vertex by where it is and its UV, quantised (0.01 mm, 1/65536): the render-to-description bridge's fallback. */
	struct FReferenceOrderKey
	{
		FIntVector Position = FIntVector::ZeroValue;
		FIntPoint UV = FIntPoint::ZeroValue;

		static FReferenceOrderKey Of(const FVector3f& InPosition, const FVector2f& InUV)
		{
			FReferenceOrderKey Key;
			Key.Position = FIntVector(FMath::RoundToInt32(InPosition.X * 1000.0f), FMath::RoundToInt32(InPosition.Y * 1000.0f),
				FMath::RoundToInt32(InPosition.Z * 1000.0f));
			Key.UV = FIntPoint(FMath::RoundToInt32(InUV.X * 65536.0f), FMath::RoundToInt32(InUV.Y * 65536.0f));
			return Key;
		}

		bool operator==(const FReferenceOrderKey& Other) const
		{
			return Position == Other.Position && UV == Other.UV;
		}

		friend uint32 GetTypeHash(const FReferenceOrderKey& Key)
		{
			return HashCombineFast(GetTypeHash(Key.Position), GetTypeHash(Key.UV));
		}
	};

	/**
	 * Each description vertex's render vertex (by ReadDescriptionVertices' order): from the LOD model's
	 * MeshToImportVertexMap (render vertex to description vertex id) when each instance is on its own vertex
	 * (bVertexPerInstance, so vertex ids are that order) and the map is a permutation of the N vertices, else by matching
	 * positions and UVs. False when neither gives a permutation. OutHow names the bridge, for the log.
	 */
	bool RenderOfDescription(const USkeletalMesh& Mesh, const int32 LODIndex, const bool bVertexPerInstance, TConstArrayView<FVector3f> DescriptionPositions,
		TConstArrayView<FVector2f> DescriptionUVs, TConstArrayView<FVector3f> RenderPositions, TConstArrayView<FVector2f> RenderUVs,
		TArray<int32>& OutRenderOfReference, const TCHAR*& OutHow)
	{
		const int32 N = DescriptionPositions.Num();
		OutRenderOfReference.Init(INDEX_NONE, N);
		if (RenderPositions.Num() != N || RenderUVs.Num() != N)
		{
			return false;
		}
		const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
		const FSkeletalMeshLODModel* Model = Imported && Imported->LODModels.IsValidIndex(LODIndex) ? &Imported->LODModels[LODIndex] : nullptr;
		if (bVertexPerInstance && Model && Model->MeshToImportVertexMap.Num() == N)
		{
			bool bPermutation = true;
			for (int32 Render = 0; Render < N && bPermutation; ++Render)
			{
				const int32 Reference = Model->MeshToImportVertexMap[Render];
				bPermutation = Reference >= 0 && Reference < N && OutRenderOfReference[Reference] == INDEX_NONE;
				if (bPermutation)
				{
					OutRenderOfReference[Reference] = Render;
				}
			}
			if (bPermutation)
			{
				OutHow = TEXT("MeshToImportVertexMap");
				return true;
			}
			OutRenderOfReference.Init(INDEX_NONE, N);
		}
		TMap<FReferenceOrderKey, int32> ReferenceOfKey;
		ReferenceOfKey.Reserve(N);
		for (int32 Reference = 0; Reference < N; ++Reference)
		{
			const int32 Before = ReferenceOfKey.Num();
			ReferenceOfKey.Add(FReferenceOrderKey::Of(DescriptionPositions[Reference], DescriptionUVs[Reference]), Reference);
			if (ReferenceOfKey.Num() == Before)
			{
				// Two description vertices in one place with one UV: no bridge can tell them apart.
				return false;
			}
		}
		for (int32 Render = 0; Render < N; ++Render)
		{
			const int32* Reference = ReferenceOfKey.Find(FReferenceOrderKey::Of(RenderPositions[Render], RenderUVs[Render]));
			if (!Reference || OutRenderOfReference[*Reference] != INDEX_NONE)
			{
				return false;
			}
			OutRenderOfReference[*Reference] = Render;
		}
		OutHow = TEXT("positions and UVs");
		return true;
	}
}

bool FMantleCosmeticExport::ReadDescriptionVertices(const USkeletalMesh* Mesh, const int32 LODIndex, TArray<FVector3f>& OutPositions,
	TArray<FVector2f>& OutUVs, bool* bOutVertexPerInstance)
{
	OutPositions.Reset();
	OutUVs.Reset();
	if (bOutVertexPerInstance)
	{
		*bOutVertexPerInstance = false;
	}
	if (!Mesh || LODIndex < 0 || LODIndex >= Mesh->GetLODNum() || !Mesh->HasMeshDescription(LODIndex))
	{
		return false;
	}
	const FMeshDescription* Description = Mesh->GetMeshDescription(LODIndex);
	if (!Description)
	{
		return false;
	}
	// By vertex instance: one per cooked vertex, made in the cooked order, ids from 0 with no holes. An imported mesh
	// shares instances between triangles and welds vertices, so its instances outnumber its render vertices or are not
	// one run: it never reads as one.
	const int32 N = Description->VertexInstances().Num();
	if (N == 0 || Description->VertexInstances().GetArraySize() != N)
	{
		return false;
	}
	FSkeletalMeshConstAttributes Attributes(*Description);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	if (!Positions.IsValid() || !UVs.IsValid() || UVs.GetNumChannels() < 1)
	{
		return false;
	}
	bool bVertexPerInstance = Description->Vertices().Num() == N;
	OutPositions.SetNumUninitialized(N);
	OutUVs.SetNumUninitialized(N);
	for (int32 Index = 0; Index < N; ++Index)
	{
		const FVertexInstanceID Instance(Index);
		if (!Description->IsVertexInstanceValid(Instance))
		{
			OutPositions.Reset();
			OutUVs.Reset();
			return false;
		}
		const FVertexID Vertex = Description->GetVertexInstanceVertex(Instance);
		bVertexPerInstance = bVertexPerInstance && Vertex.GetValue() == Index;
		OutPositions[Index] = Positions.Get(Vertex);
		OutUVs[Index] = UVs.Get(Instance, 0);
	}
	if (bOutVertexPerInstance)
	{
		*bOutVertexPerInstance = bVertexPerInstance;
	}
	return true;
}

bool FMantleCosmeticExport::HasReferenceTopology(const USkeletalMesh* Mesh, const int32 LODIndex, const FMantleAvatarContract& Contract,
	bool* bOutDescriptionOrder, TArray<int32>* OutRenderOfReference)
{
	using namespace MantleCosmeticExportPrivate;
	if (bOutDescriptionOrder)
	{
		*bOutDescriptionOrder = false;
	}
	if (OutRenderOfReference)
	{
		OutRenderOfReference->Reset();
	}
	FMantleBodyLODTopology Topology;
	TArray<FVector3f> RenderPositions;
	TArray<FVector2f> RenderUVs;
	if (!Mesh || Contract.TopologyVertexCount <= 0 || !UMantleBodyTopologyData::ReadLOD(Mesh, LODIndex, Topology, &RenderPositions, &RenderUVs)
		|| Topology.VertexCount < Contract.TopologyVertexCount)
	{
		return false;
	}
	// MetaHuman Creator rebuilds can reorder both render vertices and mesh-description vertices.
	// Match the actual UV/connectivity layout, retaining every authored vertex attribute. The
	// plugin carries no reference body geometry, and a shared count or skeleton is insufficient.
	if (const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::Get();
		Layout && Layout->QuantizedUVs.Num() == Contract.TopologyVertexCount && Layout->UVHash == Contract.TopologyUVHash)
	{
		const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		TArray<uint32> Indices;
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex)) { return false; }
		RenderData->LODRenderData[LODIndex].MultiSizeIndexContainer.GetIndexBuffer(Indices);
		TArray<int32> RenderOfReference;
		FString Error;
		if (!Layout->Match(RenderUVs, Indices, RenderOfReference, Error))
		{
			// MetaHuman Creator's in-memory body (the standard body types): the reference layout before Epic's Optimized build cut a
			// few quads the other way (FMantleBodyCorrespondence::MatchFlippedDiagonals). Only for the meshes Make fetched.
			FString FlipNote;
			if (FMantleFitBodies::IsCreatorPreview(Mesh) && Layout->MatchFlippedDiagonals(RenderUVs, Indices, RenderOfReference, FlipNote, Error))
			{
			}
			else
			{
				TArray<int32> ReferenceOfInput; TArray<FString> MergedSplits;
				if (LODIndex != 0 || !Layout->MatchMergedBodyExport(Mesh, ReferenceOfInput, RenderOfReference, MergedSplits, Error)) return false;
			}
		}
		bool bReordered = false;
		for (int32 Vertex = 0; Vertex < RenderOfReference.Num(); ++Vertex)
		{
			bReordered |= RenderOfReference[Vertex] != Vertex;
		}
		if (bOutDescriptionOrder) { *bOutDescriptionOrder = bReordered; }
		if (OutRenderOfReference) { *OutRenderOfReference = MoveTemp(RenderOfReference); }
		return true;
	}
	if (Topology.VertexCount != Contract.TopologyVertexCount) return false;
	// The production contract requires the complete layout. A missing/invalid resource must
	// not quietly downgrade acceptance to a UV checksum; custom test contracts retain their bridge.
	if (const FMantleAvatarContract* Production = FMantleAvatarContract::Get();
		Production && Production->TopologyVertexCount == Contract.TopologyVertexCount && Production->TopologyUVHash == Contract.TopologyUVHash)
	{
		return false;
	}
	if (Topology.UVHash == Contract.TopologyUVHash)
	{
		// Render order: an imported body.
		return true;
	}

	// Description order: the description itself (an imported mesh's has another count of instances, and is refused there).
	TArray<FVector3f> DescriptionPositions;
	TArray<FVector2f> DescriptionUVs;
	bool bVertexPerInstance = false;
	if (!ReadDescriptionVertices(Mesh, LODIndex, DescriptionPositions, DescriptionUVs, &bVertexPerInstance)
		|| DescriptionPositions.Num() != Contract.TopologyVertexCount || UMantleBodyTopologyData::ComputeUVHash(DescriptionUVs) != Contract.TopologyUVHash)
	{
		return false;
	}
	// The bridge for render-order consumers, checked on the render data itself.
	TArray<int32> RenderOfReference;
	const TCHAR* How = TEXT("");
	if (!RenderOfDescription(*Mesh, LODIndex, bVertexPerInstance, DescriptionPositions, DescriptionUVs, RenderPositions, RenderUVs, RenderOfReference, How))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_REFERENCE_TOPOLOGY %s LOD %d: its description has the reference topology, but no render vertex maps to each of its vertices"),
			*Mesh->GetName(), LODIndex);
		return false;
	}
	TArray<FVector2f> Reordered;
	Reordered.SetNumUninitialized(RenderOfReference.Num());
	for (int32 Reference = 0; Reference < RenderOfReference.Num(); ++Reference)
	{
		Reordered[Reference] = RenderUVs[RenderOfReference[Reference]];
	}
	if (UMantleBodyTopologyData::ComputeUVHash(Reordered) != Contract.TopologyUVHash)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_REFERENCE_TOPOLOGY %s LOD %d: its render data read through %s is not the reference topology"),
			*Mesh->GetName(), LODIndex, How);
		return false;
	}
	if (bOutDescriptionOrder)
	{
		*bOutDescriptionOrder = true;
	}
	if (OutRenderOfReference)
	{
		*OutRenderOfReference = MoveTemp(RenderOfReference);
	}
	return true;
}

TArray<FString> FMantleCosmeticExport::SectionMaterialNames(const USkeletalMesh* Mesh, int32 LODIndex)
{
	TArray<FString> Out;
	const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return Out;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	TArray<int32> Order;
	for (int32 Section = 0; Section < LOD.RenderSections.Num(); ++Section)
	{
		Order.Add(Section);
	}
	Order.Sort([&LOD](int32 A, int32 B) { return LOD.RenderSections[A].BaseVertexIndex < LOD.RenderSections[B].BaseVertexIndex; });
	TSet<int32> Seen;
	const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
	for (const int32 Section : Order)
	{
		const int32 Slot = MantleCosmeticExportPrivate::SectionSlot(Mesh, LODIndex, Section, LOD.RenderSections[Section].MaterialIndex);
		bool bSeen = false;
		Seen.Add(Slot, &bSeen);
		if (!bSeen)
		{
			Out.Add(MantleCosmeticExportPrivate::SlotName(Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName : NAME_None,
				Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialInterface.Get() : nullptr, Slot));
		}
	}
	return Out;
}

TArray<FString> FMantleCosmeticExport::RenderSectionNames(const USkeletalMesh* Mesh, int32 LODIndex)
{
	TArray<FString> Out;
	const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return Out;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
	for (int32 Section = 0; Section < LOD.RenderSections.Num(); ++Section)
	{
		const int32 Slot = MantleCosmeticExportPrivate::SectionSlot(Mesh, LODIndex, Section, LOD.RenderSections[Section].MaterialIndex);
		Out.Add(MantleCosmeticExportPrivate::SlotName(Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName : NAME_None,
			Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialInterface.Get() : nullptr, Slot));
	}
	return Out;
}
TArray<FString> FMantleCosmeticExport::DefaultSectionRoles(const USkeletalMesh* Mesh, int32 LODIndex)
{
	TArray<FString> Out;
	for (const FString& Name : SectionMaterialNames(Mesh, LODIndex))
	{
		Out.Add(MantleCosmeticExportPrivate::DefaultRole(Name));
	}
	return Out;
}

bool FMantleCosmeticExport::BuildReducedLODMaterialMap(TConstArrayView<int32> SourceSectionSlots,
	TConstArrayView<int32> GeneratedMaterialIndices, TConstArrayView<int32> ExistingLODMaterialMap, const int32 MaterialSlotCount,
	TArray<int32>& OutLODMaterialMap, FString& OutError)
{
	OutLODMaterialMap.Reset();
	OutError.Reset();
	if (SourceSectionSlots.Num() <= 0 || GeneratedMaterialIndices.Num() <= 0 || MaterialSlotCount <= 0)
	{
		OutError = TEXT("The source sections, generated sections and material slots must be present.");
		return false;
	}
	OutLODMaterialMap.Reserve(GeneratedMaterialIndices.Num());
	for (int32 GeneratedSection = 0; GeneratedSection < GeneratedMaterialIndices.Num(); ++GeneratedSection)
	{
		const int32 SourceSection = GeneratedMaterialIndices[GeneratedSection];
		if (!SourceSectionSlots.IsValidIndex(SourceSection))
		{
			OutLODMaterialMap.Reset();
			OutError = FString::Printf(TEXT("Generated section %d refers to source section %d, outside its section map."), GeneratedSection, SourceSection);
			return false;
		}
		const int32 MaterialSlot = SourceSectionSlots[SourceSection];
		if (MaterialSlot < 0 || MaterialSlot >= MaterialSlotCount)
		{
			OutLODMaterialMap.Reset();
			OutError = FString::Printf(TEXT("Source section %d resolves to invalid material slot %d."), SourceSection, MaterialSlot);
			return false;
		}
		if (ExistingLODMaterialMap.IsValidIndex(GeneratedSection) && ExistingLODMaterialMap[GeneratedSection] != INDEX_NONE
			&& ExistingLODMaterialMap[GeneratedSection] != MaterialSlot)
		{
			OutLODMaterialMap.Reset();
			OutError = FString::Printf(TEXT("Generated section %d's existing slot %d conflicts with source section %d's slot %d."),
				GeneratedSection, ExistingLODMaterialMap[GeneratedSection], SourceSection, MaterialSlot);
			return false;
		}
		OutLODMaterialMap.Add(MaterialSlot);
	}
	return true;
}

bool FMantleCosmeticExport::BuildClientAppearanceMaterials(const FMantleGlbMesh& Mesh, const TArray<FString>& Roles, const bool bFace,
	TArray<FMantleCosmeticMaterialRef>& OutMaterials, FString& OutError)
{
	OutMaterials.Reset();
	OutError.Reset();
	if (Mesh.MaterialNames.IsEmpty() || Roles.Num() != Mesh.MaterialNames.Num())
	{
		OutError = TEXT("The converted mesh's material-slot and role tables disagree.");
		return false;
	}
	TSet<FString> Seen;
	int32 BodySkin = 0, FaceSkin = INDEX_NONE, LeftEye = INDEX_NONE, RightEye = INDEX_NONE;
	for (int32 Index = 0; Index < Mesh.MaterialNames.Num(); ++Index)
	{
		FString ClientMaterial, ClientRole;
		if (!MantleCosmeticExportPrivate::ResolveClientAppearanceMaterial(Mesh.MaterialNames[Index], Roles[Index], bFace,
			ClientMaterial, ClientRole, OutError))
		{
			OutMaterials.Reset();
			return false;
		}
		if (Seen.Contains(ClientMaterial))
		{
			OutError = FString::Printf(TEXT("Client material %s maps from more than one converted section."), *ClientMaterial);
			OutMaterials.Reset();
			return false;
		}
		Seen.Add(ClientMaterial);
		FMantleCosmeticMaterialRef Ref;
		Ref.ClientMaterial = ClientMaterial;
		Ref.Role = ClientRole;
		OutMaterials.Add(MoveTemp(Ref));
		if (ClientMaterial == TEXT("skin_body_v1")) ++BodySkin;
		if (ClientMaterial == TEXT("skin_face_v1")) FaceSkin = Index;
		if (ClientMaterial == TEXT("eye_left_v1")) LeftEye = Index;
		if (ClientMaterial == TEXT("eye_right_v1")) RightEye = Index;
	}
	if ((!bFace && BodySkin != 1) || (bFace && (FaceSkin == INDEX_NONE || LeftEye == INDEX_NONE || RightEye == INDEX_NONE)))
	{
		OutError = bFace
			? TEXT("A converted MetaHuman face needs one skin section and independently bound left and right eye sections.")
			: TEXT("A converted MetaHuman body needs exactly one supported skin section.");
		OutMaterials.Reset();
		return false;
	}
	if (bFace && (!MantleCosmeticExportPrivate::HasClientEyeSideWeights(Mesh, LeftEye, true, OutError)
		|| !MantleCosmeticExportPrivate::HasClientEyeSideWeights(Mesh, RightEye, false, OutError)))
	{
		OutMaterials.Reset();
		return false;
	}
	if (bFace)
	{
		for (int32 Index = 0; Index < OutMaterials.Num(); ++Index)
		{
			if (!MantleCosmeticExportPrivate::HasClientAppearanceUVProfile(Mesh, Index, OutMaterials[Index].ClientMaterial,
				*OutMaterials[Index].ClientMaterial, OutError))
			{
				OutMaterials.Reset();
				return false;
			}
		}
	}
	return true;
}

int32 FMantleCosmeticExport::FaceSectionTextureLimit(int32 ClassLimit, const FString& Role, const FString& Section)
{
	// Teeth are intentionally catalogued as "other"; do not reduce unrelated eye edges,
	// lashes or arbitrary "other" sections. This policy is called only for avatar face materials.
	const bool bAuxiliary = Role == TEXT("eyes") || (Role == TEXT("other") && Section.Contains(TEXT("teeth"), ESearchCase::IgnoreCase));
	return bAuxiliary ? (ClassLimit > 0 ? FMath::Min(ClassLimit, 1024) : 1024) : ClassLimit;
}

USkeletalMesh* FMantleCosmeticExport::FindAuthoringBody(const UObject* Near, const FMantleAvatarContract& Contract, const bool bRequireCanonicalTopology)
{
	if (!bRequireCanonicalTopology)
	{
		FString HeadError; bool HasAssociation = false;
		if (USkeletalMesh* Head = FMantleHairSourceDiscovery::FindHead(Near,HeadError,&HasAssociation)) return Head;
		if (HasAssociation) return nullptr;
		// Unknown/ambiguous hair provenance must not silently select an unrelated global body.
		// A character-folder fallback is limited to that exact character's own body.
		const FString SourcePackage = Near ? Near->GetOutermost()->GetName() : FString();
		if (!SourcePackage.StartsWith(TEXT("/Game/MetaHumans/")) && !SourcePackage.StartsWith(TEXT("/Game/CharacterAssemblies/"))) return nullptr;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Meshes;
	Registry.GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Meshes, true);

	// "/Game/MetaHumans/<Name>/" (or "/Game/CharacterAssemblies/<Name>/") of the source, when it lives in a MetaHuman's folder.
	const FString NearFolder = Near ? MetaHumanFolder(Near->GetOutermost()->GetName()) : FString();
	const FString NearRoot = NearFolder.IsEmpty() ? FString() : NearFolder + TEXT("/");

	struct FCandidate
	{
		FAssetData Data;
		int32 Score = 0;
	};
	TArray<FCandidate> Candidates;
	for (const FAssetData& Data : Meshes)
	{
		FString Skeleton;
		Data.GetTagValue(TEXT("Skeleton"), Skeleton);
		const FString Name = Data.AssetName.ToString();
		const FString Package = Data.PackageName.ToString();
		if (!bRequireCanonicalTopology && !Package.StartsWith(NearRoot)) continue;
		if (!CouldBeAuthoringBody(Name, Package, Skeleton))
		{
			continue;
		}
		FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Data = Data;
		Candidate.Score = (!NearRoot.IsEmpty() && Package.StartsWith(NearRoot) ? 4 : 0) + (Name.Contains(TEXT("Body")) ? 2 : 0);
	}
	Candidates.StableSort([](const FCandidate& A, const FCandidate& B)
	{
		return A.Score != B.Score ? A.Score > B.Score : A.Data.PackageName.LexicalLess(B.Data.PackageName);
	});
	USkeletalMesh* HairBody = nullptr;
	for (int32 Index = 0; Index < (bRequireCanonicalTopology ? FMath::Min(8, Candidates.Num()) : Candidates.Num()); ++Index)
	{
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Candidates[Index].Data.GetAsset());
		if (Mesh && (bRequireCanonicalTopology ? FindReferenceLOD(Mesh, Contract) != INDEX_NONE : Mesh->GetRefSkeleton().FindBoneIndex(TEXT("head")) != INDEX_NONE))
		{
			if (bRequireCanonicalTopology) return Mesh;
			if (HairBody && HairBody != Mesh) return nullptr; // Multiple actual character bodies still require an explicit choice.
			HairBody = Mesh;
		}
	}
	return HairBody;
}

USkeletalMesh* FMantleCosmeticExport::FindAuthoringFace(const FMantleCosmeticDraft& Draft, const USkeletalMesh* Body)
{
	if (USkeletalMesh* Face = Draft.AuthoringFace.Get())
	{
		return Face;
	}
	if (Draft.Class == EMantleCosmeticClass::Hair && FMantleHairSourceDiscovery::IsHead(Body)) return const_cast<USkeletalMesh*>(Body);
	if (!Body)
	{
		return nullptr;
	}
	// A face mesh in the body's MetaHuman folder ("/Game/MetaHumans/<Name>" or a Cinematic MetaHuman's
	// "/Game/CharacterAssemblies/<Name>" with the face under Face/): PickAuthoringFace.
	const FString Folder = MetaHumanFolder(Body->GetOutermost()->GetName());
	if (Folder.IsEmpty())
	{
		return nullptr;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Folder));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	TArray<FAssetData> Meshes;
	Registry.GetAssets(Filter, Meshes);
	TArray<FMantleFaceCandidate> Candidates;
	for (const FAssetData& Data : Meshes)
	{
		FMantleFaceCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.PackageName = Data.PackageName.ToString();
		Candidate.AssetName = Data.AssetName.ToString();
		Data.GetTagValue(TEXT("Skeleton"), Candidate.Skeleton);
	}
	const int32 Best = PickAuthoringFace(Body->GetName(), Candidates);
	return Meshes.IsValidIndex(Best) ? Cast<USkeletalMesh>(Meshes[Best].GetAsset()) : nullptr;
}

FString FMantleCosmeticExport::MetaHumanFolder(const FString& PackageName)
{
	TArray<FString> Parts;
	PackageName.ParseIntoArray(Parts, TEXT("/"));
	if (Parts.Num() < 4)
	{
		return FString();
	}
	return FString::Printf(TEXT("/%s/%s/%s"), *Parts[0], *Parts[1], *Parts[2]);
}

bool FMantleCosmeticExport::CouldBeAuthoringBody(const FString& AssetName, const FString& PackageName, const FString& Skeleton)
{
	if (!Skeleton.Contains(TEXT("metahuman_base_skel"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	// Garments share the body skeleton: never offer an outfit as the body (an assembly's outfit sits under Clothing/).
	return !MantleCosmeticExportPrivate::NameMatches(AssetName, { TEXT("Outfit"), TEXT("Cloth"), TEXT("Shirt"), TEXT("Pants"), TEXT("Shoe"), TEXT("Top"), TEXT("Bottom"), TEXT("Face") })
		&& !PackageName.Contains(TEXT("/Clothing/"));
}

bool FMantleCosmeticExport::CouldBeUEFNMannequin(const FString& Skeleton)
{
	return Skeleton.Contains(TEXT("UEFN_Mannequin"), ESearchCase::IgnoreCase);
}

bool FMantleCosmeticExport::HasFixtureBones(const USkeleton* Skeleton, const FMantleContractSkeleton& Fixture, FString* OutMissing)
{
	if (!Skeleton)
	{
		if (OutMissing)
		{
			*OutMissing = TEXT("no skeleton");
		}
		return false;
	}
	// FindBoneIndex sees virtual bones too, but a mesh's copy may not carry them: they are skipped either way.
	const FReferenceSkeleton& Reference = Skeleton->GetReferenceSkeleton();
	for (const FName Bone : Fixture.Bones)
	{
		if (!Bone.ToString().StartsWith(TEXT("VB "), ESearchCase::CaseSensitive) && Reference.FindBoneIndex(Bone) == INDEX_NONE)
		{
			if (OutMissing)
			{
				*OutMissing = Bone.ToString();
			}
			return false;
		}
	}
	return true;
}

USkeletalMesh* FMantleCosmeticExport::FindUEFNMannequin(const FMantleAvatarContract& Contract)
{
	const FMantleContractSkeleton* Fixture = Contract.FindSkeleton(Contract.EmoteSkeleton);
	if (!Fixture)
	{
		return nullptr;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Meshes;
	Registry.GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Meshes, true);
	TArray<FAssetData> Candidates;
	for (const FAssetData& Data : Meshes)
	{
		FString Skeleton;
		Data.GetTagValue(TEXT("Skeleton"), Skeleton);
		if (CouldBeUEFNMannequin(Skeleton))
		{
			Candidates.Add(Data);
		}
	}
	// GASP's and UEFN's own mesh first (the one the manual retarget path names), then by path: the same pick every time.
	Candidates.StableSort([](const FAssetData& A, const FAssetData& B)
	{
		const bool bA = A.AssetName.ToString().Equals(TEXT("SKM_UEFN_Mannequin"), ESearchCase::CaseSensitive);
		const bool bB = B.AssetName.ToString().Equals(TEXT("SKM_UEFN_Mannequin"), ESearchCase::CaseSensitive);
		return bA != bB ? bA : A.PackageName.LexicalLess(B.PackageName);
	});
	for (int32 Index = 0; Index < FMath::Min(8, Candidates.Num()); ++Index)
	{
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Candidates[Index].GetAsset());
		FString Missing;
		if (Mesh && HasFixtureBones(Mesh->GetSkeleton(), *Fixture, &Missing))
		{
			return Mesh;
		}
		if (Mesh)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_UEFN_MANNEQUIN %s is not a UEFN mannequin: its skeleton has no bone %s"), *Mesh->GetPathName(), *Missing);
		}
	}
	return nullptr;
}

int32 FMantleCosmeticExport::PickAuthoringFace(const FString& BodyName, TConstArrayView<FMantleFaceCandidate> Candidates)
{
	// The one whose name shares the most with the body's ("SKM_Character" goes with "SKM_Character_Face",
	// "<Name>_BodyMesh" with "<Name>_FaceMesh", "SKM_<Name>_Body" with "SKM_<Name>_Face"), then the first by path.
	// A face named for its character alone ("SKM_Kid_01", "SKM_Kid_04MetahumanMesh") counts when it is on the face skeleton
	// in the character's Face/ folder, after any named one.
	TArray<int32> ByPath;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		ByPath.Add(Index);
	}
	ByPath.StableSort([Candidates](int32 A, int32 B) { return Candidates[A].PackageName < Candidates[B].PackageName; });
	int32 Best = INDEX_NONE;
	int32 BestNamed = -1;
	int32 BestShared = -1;
	for (const int32 Index : ByPath)
	{
		const FMantleFaceCandidate& Candidate = Candidates[Index];
		if (!Candidate.Skeleton.Contains(TEXT("Face")))
		{
			continue;
		}
		const int32 Named = Candidate.AssetName.Contains(TEXT("Face")) ? 1 : 0;
		if (!Named && !Candidate.PackageName.Contains(TEXT("/Face/"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 Shared = MantleCosmeticExportPrivate::CommonPrefix(Candidate.AssetName, BodyName);
		if (Named > BestNamed || (Named == BestNamed && Shared > BestShared))
		{
			Best = Index;
			BestNamed = Named;
			BestShared = Shared;
		}
	}
	return Best;
}

bool FMantleCosmeticExport::IsGreyPng(const TArray<uint8>& Png)
{
	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Reader = Module.CreateImageWrapper(EImageFormat::PNG);
	TArray64<uint8> Pixels;
	if (!Reader.IsValid() || !Reader->SetCompressed(Png.GetData(), Png.Num()) || !Reader->GetRaw(ERGBFormat::RGBA, 8, Pixels))
	{
		return false;
	}
	for (int64 Index = 0; Index + 3 < Pixels.Num(); Index += 4)
	{
		if (Pixels[Index] != Pixels[Index + 1] || Pixels[Index + 1] != Pixels[Index + 2])
		{
			return false;
		}
	}
	return true;
}

bool FMantleCosmeticExport::TagPngRole(TArray<uint8>& Png, const FString& Role)
{
	// The signature, then IHDR: a 4-byte length (13), "IHDR", 13 bytes, a 4-byte CRC; the chunk goes after it (offset 33).
	static const uint8 Signature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	constexpr int32 AfterHeader = 33;
	if (Png.Num() < AfterHeader || FMemory::Memcmp(Png.GetData(), Signature, 8) != 0 || FMemory::Memcmp(Png.GetData() + 12, "IHDR", 4) != 0)
	{
		return false;
	}
	const FTCHARToUTF8 Value(*Role);
	TArray<uint8> Chunk;
	const char Keyword[] = "Mantle-Role";
	const int32 DataLength = static_cast<int32>(sizeof(Keyword)) + Value.Length(); // the keyword's terminating zero separates the two
	Chunk.Reserve(12 + DataLength);
	for (const int32 Shift : { 24, 16, 8, 0 })
	{
		Chunk.Add(static_cast<uint8>((DataLength >> Shift) & 0xFF));
	}
	Chunk.Append(reinterpret_cast<const uint8*>("tEXt"), 4);
	Chunk.Append(reinterpret_cast<const uint8*>(Keyword), sizeof(Keyword));
	Chunk.Append(reinterpret_cast<const uint8*>(Value.Get()), Value.Length());
	// PNG's CRC-32 (ISO 3309, reflected 0xEDB88320) over the type and the data.
	uint32 Crc = 0xFFFFFFFFu;
	for (int32 Index = 4; Index < Chunk.Num(); ++Index)
	{
		Crc ^= Chunk[Index];
		for (int32 Bit = 0; Bit < 8; ++Bit)
		{
			Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u)));
		}
	}
	Crc ^= 0xFFFFFFFFu;
	for (const int32 Shift : { 24, 16, 8, 0 })
	{
		Chunk.Add(static_cast<uint8>((Crc >> Shift) & 0xFF));
	}
	Png.Insert(Chunk, AfterHeader);
	return true;
}

bool FMantleCosmeticExport::TextureToPng(const UTexture* Texture, int32 MaxSize, bool bLinear, TArray<uint8>& OutPng, FIntPoint& OutSize, FString& OutError)
{
	OutPng.Reset();
	TArray<FColor> Pixels;
	if (!MantleCosmeticExportPrivate::ReadTexturePixels(Texture, MaxSize, bLinear, Pixels, OutSize, OutError))
	{
		return false;
	}
	OutPng = FMantlePng::EncodeRGBA8(OutSize.X, OutSize.Y, Pixels);
	if (OutPng.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s could not be encoded as PNG"), *Texture->GetName());
		return false;
	}
	return true;
}

void FMantleCosmeticExport::CheckSourceTextureSize(const UTexture* Texture, int32 MaxSize, const FString& What,
	const FMantleAvatarContract& Contract, TArray<FMantleCosmeticIssue>& OutIssues)
{
	if (!Texture || !Texture->Source.IsValid())
	{
		return;
	}
	const int32 Width = static_cast<int32>(FMath::Min<int64>(Texture->Source.GetSizeX(), MAX_int32));
	const int32 Height = static_cast<int32>(FMath::Min<int64>(Texture->Source.GetSizeY(), MAX_int32));
	if (Width <= 0 || Height <= 0)
	{
		return;
	}
	FMantleCosmeticChecks::CheckTextureSize(What, Width, Height, MaxSize, Contract, OutIssues);
}

int32 FMantleCosmeticExport::CountSplitVertices(TConstArrayView<uint32> Indices, TConstArrayView<FMantleSharedVertexSection> Sections)
{
	int32 Total = 0;
	for (const FMantleSharedVertexSection& Section : Sections)
	{
		TSet<uint32> Used;
		const int64 End = FMath::Min<int64>(static_cast<int64>(Section.FirstIndex) + static_cast<int64>(Section.NumTriangles) * 3, Indices.Num());
		for (int64 Index = Section.FirstIndex; Index < End; ++Index)
		{
			Used.Add(Indices[static_cast<int32>(Index)]);
		}
		Total += Used.Num();
	}
	return Total;
}

void FMantleCosmeticExport::SplitSharedVertexSections(FMantleGlbMesh& Glb, TConstArrayView<uint32> Indices, TConstArrayView<FMantleSharedVertexSection> Sections)
{
	// Copies of the old arrays: the new ones are built from them section by section.
	const TArray<FVector3f> OldPositions = MoveTemp(Glb.Positions);
	const TArray<FVector3f> OldNormals = MoveTemp(Glb.Normals);
	const TArray<FVector4f> OldTangents = MoveTemp(Glb.Tangents);
	const TArray<FVector2f> OldUVs = MoveTemp(Glb.UVs);
	const TArray<FVector2f> OldUV1s = MoveTemp(Glb.UV1s);
	const TArray<FColor> OldColors = MoveTemp(Glb.Colors);
	Glb.Positions.Reset();
	Glb.Normals.Reset();
	Glb.Tangents.Reset();
	Glb.UVs.Reset();
	Glb.UV1s.Reset();
	Glb.Colors.Reset();

	for (const FMantleSharedVertexSection& Section : Sections)
	{
		FMantleGlbPrimitive& Primitive = Glb.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = Section.GlbMaterial;
		Primitive.FirstVertex = Glb.Positions.Num();

		TMap<uint32, uint32> NewIndexOf;
		const int64 End = FMath::Min<int64>(static_cast<int64>(Section.FirstIndex) + static_cast<int64>(Section.NumTriangles) * 3, Indices.Num());
		for (int64 At = Section.FirstIndex; At < End; ++At)
		{
			const uint32 Old = Indices[static_cast<int32>(At)];
			const uint32* Found = NewIndexOf.Find(Old);
			if (!Found)
			{
				// Vertices in the order the section first uses them.
				const uint32 New = static_cast<uint32>(Glb.Positions.Num() - Primitive.FirstVertex);
				Found = &NewIndexOf.Add(Old, New);
				Glb.Positions.Add(OldPositions[Old]);
				Glb.Normals.Add(OldNormals[Old]);
				if (OldTangents.Num() > 0)
				{
					Glb.Tangents.Add(OldTangents[Old]);
				}
				if (OldUVs.Num() > 0)
				{
					Glb.UVs.Add(OldUVs[Old]);
				}
				if (OldUV1s.Num() > 0)
				{
					Glb.UV1s.Add(OldUV1s[Old]);
				}
				if (OldColors.Num() > 0)
				{
					Glb.Colors.Add(OldColors[Old]);
				}
			}
			Primitive.Indices.Add(*Found);
		}
		Primitive.NumVertices = Glb.Positions.Num() - Primitive.FirstVertex;
	}
}

bool FMantleCosmeticExport::ReadStaticMesh(const UStaticMesh* Mesh, int32 LODIndex, FMantleGlbMesh& OutMesh, TArray<int32>& OutSlots,
	int32& OutTriangles, FString& OutError)
{
	MantleCosmeticExportPrivate::FMeshSource Source;
	OutMesh = FMantleGlbMesh();
	OutSlots.Reset();
	OutTriangles = 0;
	if (!MantleCosmeticExportPrivate::ReadStaticLOD(Mesh, LODIndex, Source, OutError))
	{
		return false;
	}
	OutMesh = MoveTemp(Source.Glb);
	OutSlots = MoveTemp(Source.SlotIndices);
	OutTriangles = Source.NumTriangles;
	return true;
}

bool FMantleCosmeticExport::ReadSkeletalMesh(const USkeletalMesh* Mesh, int32 LODIndex, FMantleGlbMesh& OutMesh, TArray<int32>& OutSlots,
	int32& OutTriangles, int32& OutCappedVertices, float& OutLargestDropped, FString& OutError)
{
	MantleCosmeticExportPrivate::FMeshSource Source;
	OutMesh = FMantleGlbMesh();
	OutSlots.Reset();
	OutTriangles = 0;
	OutCappedVertices = 0;
	OutLargestDropped = 0.0f;
	if (!MantleCosmeticExportPrivate::ReadSkeletalLOD(Mesh, LODIndex, Source, OutError))
	{
		return false;
	}
	OutMesh = MoveTemp(Source.Glb);
	OutSlots = MoveTemp(Source.SlotIndices);
	OutTriangles = Source.NumTriangles;
	OutCappedVertices = Source.CappedVertices;
	OutLargestDropped = Source.LargestDroppedWeight;
	return true;
}

TArray<int32> FMantleCosmeticExport::UploadMaterialSlots(const USkeletalMesh* Mesh, int32 LODIndex)
{
	// ReadSkeletalLOD's order: sections by vertex (KeptSections), each one's slot the first time it is drawn.
	TArray<int32> Out;
	if (!Mesh)
	{
		return Out;
	}
	const TArray<FMantleSectionSpan> Spans = ReadSectionSpans(Mesh, LODIndex);
	for (const int32 Section : KeptSections(Spans, nullptr))
	{
		if (Spans.IsValidIndex(Section))
		{
			Out.AddUnique(Spans[Section].Slot);
		}
	}
	return Out;
}

bool FMantleCosmeticExport::ReadSkeletalMesh(const USkeletalMesh* Mesh, int32 LODIndex, FMantleGlbMesh& OutMesh,
	TArray<UMaterialInterface*>& OutMaterials, int32& OutTriangles, FString& OutError)
{
	MantleCosmeticExportPrivate::FMeshSource Source;
	OutMesh = FMantleGlbMesh();
	OutMaterials.Reset();
	OutTriangles = 0;
	if (!MantleCosmeticExportPrivate::ReadSkeletalLOD(Mesh, LODIndex, Source, OutError))
	{
		return false;
	}
	OutMesh = MoveTemp(Source.Glb);
	OutMaterials = MoveTemp(Source.Materials);
	OutTriangles = Source.NumTriangles;
	return true;
}

TArray<uint8> FMantleCosmeticExport::PackCoverageBits(const TBitArray<>& Covered, int32 NumBits)
{
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed((NumBits + 7) / 8);
	for (int32 Bit = 0; Bit < NumBits && Bit < Covered.Num(); ++Bit)
	{
		if (Covered[Bit])
		{
			Bytes[Bit / 8] |= static_cast<uint8>(1u << (Bit % 8));
		}
	}
	return Bytes;
}

int32 FMantleCosmeticExport::PickOpacityTexture(TConstArrayView<FMantleTextureParameter> Parameters)
{
	using namespace MantleCosmeticExportPrivate;
	const TCHAR* const Words[] = { TEXT("Opacity"), TEXT("Alpha"), TEXT("Transparency"), TEXT("Cutout") };
	for (int32 Index = 0; Index < Parameters.Num(); ++Index)
	{
		const FMantleTextureParameter& Parameter = Parameters[Index];
		if (!Parameter.bNormalMap && !Parameter.bTaken && NameMatches(Parameter.Parameter, { Words[0], Words[1], Words[2], Words[3] }))
		{
			return Index;
		}
	}
	for (int32 Index = 0; Index < Parameters.Num(); ++Index)
	{
		const FMantleTextureParameter& Parameter = Parameters[Index];
		if (Parameter.bNormalMap || Parameter.bTaken)
		{
			continue;
		}
		for (const TCHAR* Word : Words)
		{
			if (Parameter.Texture.EndsWith(Word, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
	}
	return INDEX_NONE;
}

EMantleOpacityChannel FMantleCosmeticExport::ChooseOpacityChannel(TConstArrayView<FColor> OpacityPixels)
{
	bool bGrey = true;
	bool bRgbConstant = true;
	bool bAlphaVaries = false;
	for (const FColor& Pixel : OpacityPixels)
	{
		bGrey = bGrey && Pixel.R == Pixel.G && Pixel.G == Pixel.B;
		bRgbConstant = bRgbConstant && Pixel.R == OpacityPixels[0].R && Pixel.G == OpacityPixels[0].G && Pixel.B == OpacityPixels[0].B;
		bAlphaVaries = bAlphaVaries || Pixel.A != 255;
	}
	// Alpha when it carries something and the colour does not (a flat colour, or a colour image): a grey image whose
	// grey varies is a single-channel mask, whatever its alpha.
	return bAlphaVaries && (bRgbConstant || !bGrey) ? EMantleOpacityChannel::Alpha : EMantleOpacityChannel::Red;
}

void FMantleCosmeticExport::PackOpacityIntoAlpha(TArrayView<FColor> AlbedoPixels, TConstArrayView<FColor> OpacityPixels, EMantleOpacityChannel Channel)
{
	const int32 Count = FMath::Min(AlbedoPixels.Num(), OpacityPixels.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		AlbedoPixels[Index].A = Channel == EMantleOpacityChannel::Alpha ? OpacityPixels[Index].A : OpacityPixels[Index].R;
	}
}

bool FMantleCosmeticExport::IsAnimatedMaskTexture(const FString& Name)
{
	if (Name.Contains(TEXT("Animated"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	FString Spaced = Name;
	for (TCHAR& Character : Spaced)
	{
		Character = FChar::IsAlnum(Character) ? Character : TEXT(' ');
	}
	TArray<FString> Tokens;
	Spaced.ParseIntoArrayWS(Tokens);
	for (const FString& Token : Tokens)
	{
		// "wm1", "WM13", "cm2", "wm1a": a wrinkle map or colour mask by number; "wm" alone ("T_head_wm_msk_Array").
		const bool bWrinkle = Token.StartsWith(TEXT("wm"), ESearchCase::IgnoreCase);
		const bool bColourMask = Token.StartsWith(TEXT("cm"), ESearchCase::IgnoreCase);
		if ((bWrinkle && Token.Len() == 2) || ((bWrinkle || bColourMask) && Token.Len() > 2 && FChar::IsDigit(Token[2])))
		{
			return true;
		}
	}
	return false;
}

FMantleMaterialTextures FMantleCosmeticExport::ResolveMaterialTextures(UMaterialInterface* Material, bool bPickOpacity)
{
	using namespace MantleCosmeticExportPrivate;
	FMantleMaterialTextures Out;
	if (!Material)
	{
		// Nothing to evaluate: the upload gives it a flat grey.
		Out.bPlain = true;
		return Out;
	}

	// The graph (the base material's own nodes; functions are not entered): what drives Base Color and Normal, and which
	// textures it only ever samples tiled.
	UTexture* PlainBaseColor = nullptr;
	UTexture* PlainNormal = nullptr;
	TSet<const UTexture*> TiledTextures;
	const UMaterial* Base = Material->GetMaterial();
	if (IsMantlePlaceMaterial(Material))
	{
		// Mantle's own material (what the converter writes): its BaseColor, Normal and ORM parameters are the textures.
		Out.bPlain = true;
	}
	else if (Base)
	{
		TSet<FName> TiledParameters;
		TSet<FName> UntiledParameters;
		TSet<const UTexture*> UntiledTextures;
		for (const TObjectPtr<UMaterialExpression>& Expression : Base->GetExpressions())
		{
			const UMaterialExpressionTextureSample* Sample = AsTextureSample(Expression.Get());
			if (!Sample)
			{
				continue;
			}
			FName Parameter;
			const UTexture* Texture = SampledTexture(Sample, Material, Parameter);
			const bool bTiled = ClassifySampleUV(Sample, Material) == EMaterialSampleUV::Tiled;
			if (!Parameter.IsNone())
			{
				(bTiled ? TiledParameters : UntiledParameters).Add(Parameter);
			}
			if (Texture)
			{
				(bTiled ? TiledTextures : UntiledTextures).Add(Texture);
			}
		}
		for (const FName& Parameter : TiledParameters)
		{
			if (!UntiledParameters.Contains(Parameter))
			{
				Out.TiledParameters.Add(Parameter);
			}
		}
		for (const UTexture* Texture : UntiledTextures)
		{
			TiledTextures.Remove(Texture);
		}
		const UMaterialEditorOnlyData* Editor = Base->GetEditorOnlyData();
		if (!Editor)
		{
			Out.Why = TEXT("its material graph could not be read");
		}
		else if (Base->bUseMaterialAttributes)
		{
			Out.Why = TEXT("its material builds material attributes (layers or blended attribute sets)");
		}
		else
		{
			PlainBaseColor = PlainInputTexture(Editor->BaseColor, Material, TEXT("base colour"), Out.Why);
			Out.bPlain = PlainBaseColor != nullptr;
			FString NormalWhy;
			PlainNormal = PlainInputTexture(Editor->Normal, Material, TEXT("normal"), NormalWhy);
		}
	}
	else
	{
		Out.Why = TEXT("it has no base material");
	}

	// By parameter name, as the upload always has, never an animated wrinkle map or a detail map.
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Material->GetAllTextureParameterInfo(Infos, Ids);
	TArray<FMantleTextureParameter> Parameters;
	TArray<UTexture*> ParameterTextures;
	TArray<bool> Skipped;
	for (const FMaterialParameterInfo& Info : Infos)
	{
		UTexture* Texture = nullptr;
		if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Texture) || !Texture)
		{
			continue;
		}
		const FString Name = Info.Name.ToString();
		const bool bNormal = Texture->CompressionSettings == TC_Normalmap || NameMatches(Name, { TEXT("normal") });
		FMantleTextureParameter& Parameter = Parameters.AddDefaulted_GetRef();
		Parameter.Parameter = Name;
		Parameter.Texture = Texture->GetName();
		Parameter.bNormalMap = bNormal;
		ParameterTextures.Add(Texture);
		const bool bSkip = IsAnimatedMaskTexture(Name) || IsAnimatedMaskTexture(Texture->GetName()) || Out.TiledParameters.Contains(Info.Name)
			|| TiledTextures.Contains(Texture);
		Skipped.Add(bSkip);
		if (bSkip)
		{
			continue;
		}
		if (bNormal)
		{
			Out.Normal = Out.Normal ? Out.Normal : Texture;
			continue;
		}
		if (NameMatches(Name, { TEXT("ORM"), TEXT("RMA"), TEXT("ARM"), TEXT("OcclusionRoughnessMetal"), TEXT("SRMF"), TEXT("Roughness") }))
		{
			if (!Out.Orm)
			{
				Out.Orm = Texture;
				Out.OrmParameter = Name;
			}
			continue;
		}
		if (NameMatches(Name, { TEXT("BaseColor"), TEXT("Base Color"), TEXT("Base_Color"), TEXT("Albedo"), TEXT("Diffuse"), TEXT("Color_MAIN"), TEXT("ColorMap"), TEXT("Color") })
			&& !NameMatches(Name, { TEXT("Mask"), TEXT("Tint"), TEXT("Emissive"), TEXT("Opacity"), TEXT("Detail"), TEXT("Spec") }))
		{
			Out.BaseColor = Out.BaseColor ? Out.BaseColor : Texture;
		}
	}
	// The graph wins over names: a plain sample is exactly what the material shows.
	if (PlainBaseColor)
	{
		Out.BaseColor = PlainBaseColor;
	}
	if (PlainNormal)
	{
		Out.Normal = PlainNormal;
	}

	// A separate opacity texture, before the base colour fallback below so that never takes it.
	if (bPickOpacity)
	{
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			const UTexture* Texture = ParameterTextures[Index];
			Parameters[Index].bTaken = Skipped[Index] || Texture == Out.BaseColor || Texture == Out.Normal || Texture == Out.Orm;
		}
		const int32 OpacityIndex = PickOpacityTexture(Parameters);
		Out.Opacity = ParameterTextures.IsValidIndex(OpacityIndex) ? ParameterTextures[OpacityIndex] : nullptr;
	}

	if (!Out.BaseColor)
	{
		// The largest colour texture the material uses.
		TArray<UTexture*> Used;
		Material->GetUsedTextures(Used);
		int64 Best = 0;
		for (UTexture* Texture : Used)
		{
			if (!Texture || Texture == Out.Opacity || Texture == Out.Normal || Texture == Out.Orm || Texture->CompressionSettings == TC_Normalmap || !Texture->SRGB
				|| !Texture->Source.IsValid() || TiledTextures.Contains(Texture) || IsAnimatedMaskTexture(Texture->GetName()))
			{
				continue;
			}
			const int64 Pixels = static_cast<int64>(Texture->Source.GetSizeX()) * Texture->Source.GetSizeY();
			if (Pixels > Best)
			{
				Best = Pixels;
				Out.BaseColor = Texture;
			}
		}
		Out.bBaseColorFromLargest = Out.BaseColor != nullptr;
	}
	return Out;
}

FMantleCosmeticPackage FMantleCosmeticExport::Build(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, FMantleSkinnedMesh* OutHairPreview, FMantleSkinnedMesh* OutHairSourceFace)
{
	FMantleCosmeticPackage Package = BuildCore(Draft, Contract, OutHairPreview, OutHairSourceFace);
	// A public item says where it came from (the source asset's path and its import file's name, not the
	// folder it sat in on disk). A native outfit's variants were built from prepared copies of the draft, so this is set
	// last, from the creator's own source.
	// #54: a placed overlay comes from its picture, not a mask.
	const bool bPlaced = Draft.Class == EMantleCosmeticClass::Overlay && Draft.bOverlayPlaced;
	const UObject* ProvenanceAsset = bPlaced ? static_cast<const UObject*>(Draft.OverlayPicture.Get()) : Draft.Source.Get();
	const FString& ProvenanceFile = bPlaced ? Draft.OverlayPictureFile : Draft.OverlayMaskFile;
	Package.Provenance = Draft.bPublic ? FMantleProvenance::FromAsset(ProvenanceAsset) : FMantleProvenance();
	if (Draft.bPublic && Draft.Class == EMantleCosmeticClass::Overlay && !ProvenanceAsset && !ProvenanceFile.IsEmpty())
	{
		// A PNG file has no asset path; its file name tells a moderator where the image came from.
		Package.Provenance.ImportFilename = FPaths::GetCleanFilename(ProvenanceFile);
	}
	return Package;
}

void FMantleCosmeticExport::AddDetectedClothingColors(FMantleCosmeticPackage& Package, const FMantleGlbMesh& Mesh, TConstArrayView<UMaterialInterface*> Materials,
	const FMantleCosmeticDraft& Draft, int32 MaxTextureSize, const FMantleAvatarContract& Contract)
{
	using namespace MantleCosmeticExportPrivate;
	Package.DetectedClothingColors.Reset();
	const bool bClothing = Draft.Class == EMantleCosmeticClass::Garment && (Draft.Slot == TEXT("top") || Draft.Slot == TEXT("bottom") || Draft.Slot == TEXT("footwear"));
	if (!Draft.bDetectClothingColors || !Draft.ClothingColors.IsEmpty() || (!bClothing && Draft.Class != EMantleCosmeticClass::Accessory))
	{
		return;
	}
	int32 MainNumber = 1, AccentNumber = 1, Found = 0;
	// The item's logical colour controls: a region id declared by several sections is one control.
	TSet<FString> Declared;
	for (const FMantleCosmeticMaterialRef& Ref : Package.Materials) for (const FMantleClothingColorRegion& Region : Ref.ColorRegions) Declared.Add(Region.Id);
	for (int32 Index = 0; Index < Mesh.MaterialNames.Num() && Index < Package.Materials.Num(); ++Index)
	{
		const int32 MainBefore = MainNumber, AccentBefore = AccentNumber;
		FMantleClothingColorDetection& Detection = Package.DetectedClothingColors.AddDefaulted_GetRef();
		Detection.MaterialName = Mesh.MaterialNames[Index];
		const FString What = TEXT("Clothing ") + Detection.MaterialName;
		if (!Package.Materials[Index].ColorRegions.IsEmpty() || Package.Materials[Index].AlbedoKey.IsEmpty())
		{
			Detection.Why = TEXT("its colours are already set up");
			continue;
		}
		const FMantleCosmeticUploadAsset* AlbedoAsset = Package.FindAsset(Package.Materials[Index].AlbedoKey);
		TArray<FColor> AlbedoPixels;
		FIntPoint AlbedoSize;
		FString Error;
		if (!AlbedoAsset || !DecodePngPixels(AlbedoAsset->Bytes, MaxTextureSize, false, AlbedoPixels, AlbedoSize))
		{
			Detection.Why = TEXT("its textures could not be read");
			Package.Notes.Add(What + TEXT(": colour areas: its colour texture could not be read; original colours only."));
			continue;
		}
		// Sellers name their area maps every way ("Mask_01", "PatternMask", "Albedo_RGB_Mask"): every map named like
		// one is tried, the clearly named first, and the first whose channels hold separate areas is taken.
		TArray<FColor> Clean;
		TArray<uint8> Used;
		bool bUnchecked = false;
		for (const TPair<FString, UTexture*>& Candidate : FMantleClothingColorDetect::FindMaskTextures(Materials.IsValidIndex(Index) ? Materials[Index] : nullptr))
		{
			Detection.Parameter = Candidate.Key;
			Detection.TextureName = Candidate.Value->GetName();
			TArray<FColor> MaskPixels;
			FIntPoint MaskSize;
			Error.Reset();
			if (!ReadTexturePixels(Candidate.Value, MaxTextureSize, true, MaskPixels, MaskSize, Error))
			{
				Package.Notes.Add(FString::Printf(TEXT("%s: colour areas: %s on %s could not be read%s."), *What, *Detection.TextureName,
					*Detection.Parameter, Error.IsEmpty() ? TEXT("") : *(TEXT(" (") + Error + TEXT(")"))));
				continue;
			}
			if (MaskSize != AlbedoSize)
			{
				TArray<FColor> Resized; Resized.SetNumUninitialized(AlbedoSize.X * AlbedoSize.Y);
				FImageUtils::ImageResize(MaskSize.X, MaskSize.Y, TArrayView<const FColor>(MaskPixels), AlbedoSize.X, AlbedoSize.Y, TArrayView<FColor>(Resized), false, false);
				MaskPixels = MoveTemp(Resized);
			}
			if (!FMantleClothingColorDetect::Detect(Mesh, Index, AlbedoSize, MaskPixels, Detection, Clean, Used, MainNumber, AccentNumber, Error))
			{
				bUnchecked = true;
				Package.Notes.Add(What + TEXT(": colour areas not checked: ") + Error);
				break;
			}
			for (const FMantleAreaChannelScore& Score : Detection.Scores)
			{
				Package.Notes.Add(FString::Printf(TEXT("%s: colour areas: %s %s %c: near-binary %.3f, coverage %.3f, edge width %.2f, %s."), *What, *Detection.TextureName,
					*Detection.Parameter, TEXT("RGB")[Score.Channel], Score.Binary, Score.Coverage, Score.EdgeWidth, Score.bAccepted ? TEXT("accepted") : TEXT("rejected")));
			}
			if (!Detection.Areas.IsEmpty()) break;
		}
		if (bUnchecked)
		{
			Detection.Why = TEXT("its texture layout could not be checked");
			continue;
		}
		if (Detection.Areas.IsEmpty())
		{
			// No map with separate areas: the whole material is one colour players can change.
			Detection.Parameter.Reset();
			Detection.TextureName.Reset();
			Detection.Scores.Reset();
			if (!FMantleClothingColorDetect::DetectWhole(Mesh, Index, AlbedoSize, Detection, Clean, Used, MainNumber, Error))
			{
				Detection.Why = TEXT("its texture layout could not be checked");
				Package.Notes.Add(What + TEXT(": colour areas not checked: ") + Error);
				continue;
			}
			Package.Notes.Add(What + TEXT(": colour areas: no texture marks out separate areas, so the whole material is one colour area."));
		}
		// One material drawn by several sections (a left and right shoe) is one set of controls: its areas take the ids
		// the first such section's did, so players colour the pair together.
		int32 SharedWith = INDEX_NONE;
		for (int32 Prev = 0; Prev < Index && Materials.IsValidIndex(Index) && Materials[Index]; ++Prev)
		{
			const FMantleClothingColorDetection& Earlier = Package.DetectedClothingColors[Prev];
			if (Materials.IsValidIndex(Prev) && Materials[Prev] == Materials[Index] && Earlier.Areas.Num() == Detection.Areas.Num() && !Earlier.Areas.IsEmpty())
			{
				bool bSame = true;
				for (int32 Area = 0; Area < Detection.Areas.Num(); ++Area) bSame &= Earlier.Areas[Area].Channel == Detection.Areas[Area].Channel;
				if (!bSame) continue;
				for (int32 Area = 0; Area < Detection.Areas.Num(); ++Area)
				{
					Detection.Areas[Area].Id = Earlier.Areas[Area].Id;
					Detection.Areas[Area].Label = Earlier.Areas[Area].Label;
				}
				MainNumber = MainBefore;
				AccentNumber = AccentBefore;
				SharedWith = Prev;
				break;
			}
		}
		Found += Detection.Areas.Num();
		// Each area's swatch starts at what the item looks like there.
		for (int32 Area = 0; Area < Detection.Areas.Num(); ++Area)
		{
			// One control has one starting colour: a shared area keeps the first section's.
			Detection.Areas[Area].DefaultColor = SharedWith != INDEX_NONE ? Package.DetectedClothingColors[SharedWith].Areas[Area].DefaultColor
				: FMantleClothingColorDetect::AverageColor(AlbedoPixels, Clean, Used, Detection.Areas[Area].Channel);
		}
		TArray<FMantleClothingColorRegion> Measured;
		for (const FMantleClothingColorRegion& Region : FMantleClothingColorDetect::ApplyChoices(Detection, Draft.ClothingColorChoices))
		{
			TArray<FMantleClothingColorRegion> One = { Region };
			FString WhyNot;
			if (FMantleClothingColorMeasure::Measure(Mesh, Index, AlbedoSize, AlbedoPixels, Clean, One, WhyNot)) Measured.Add(One[0]);
			else Package.Notes.Add(FString::Printf(TEXT("%s: colour area %s keeps its original colours: %s"), *What, *Region.Label, *WhyNot));
		}
		if (Measured.IsEmpty()) continue;
		TSet<FString> WithThese = Declared;
		for (const FMantleClothingColorRegion& Region : Measured) WithThese.Add(Region.Id);
		if (WithThese.Num() > MantleClothingColor::MaxColorsPerItem)
		{
			Package.Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(TEXT("%s: players can recolour at most %d areas of one item, so this part keeps its original colours."),
				*What, MantleClothingColor::MaxColorsPerItem)));
			continue;
		}
		const TArray<uint8> Png = FMantlePng::EncodeRGBA8(AlbedoSize.X, AlbedoSize.Y, Clean);
		const FString MaskKey = AddTexture(Package, nullptr, &Png, ETextureUse::Data, MaxTextureSize, What + TEXT(" colour areas"), Contract);
		if (MaskKey.IsEmpty()) continue;
		FMantleCosmeticMaterialRef& Ref = Package.Materials[Index];
		Ref.ColorMaskKey = MaskKey;
		Ref.ColorRegions = MoveTemp(Measured);
		Declared = MoveTemp(WithThese);
		for (const FMantleClothingColorRegion& Region : Ref.ColorRegions)
		{
			Package.Notes.Add(FString::Printf(TEXT("%s: colour area %s (\"%s\") found from %s, default #%s, measured linear reference luminance %.8f."), *What, *Region.Id,
				*Region.Label, Detection.Parameter.IsEmpty() ? TEXT("the whole material") : *Detection.Parameter, *Region.DefaultColor.ToHex().Left(6), Region.ReferenceLuminance));
		}
	}
	if (Found == 0)
	{
		Package.Notes.Add(TEXT("Colour areas: none found; this item uploads in its original colours."));
	}
}

FMantleCosmeticPackage FMantleCosmeticExport::BuildCore(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, FMantleSkinnedMesh* OutHairPreview, FMantleSkinnedMesh* OutHairSourceFace)
{
	if (Draft.Class != EMantleCosmeticClass::Garment || !Draft.OutfitTargets.IsEmpty() || !FMantleParametricOutfit::IsGarmentSource(Draft.Source.Get()))
	{
		return BuildPrepared(Draft, Contract, OutHairPreview, OutHairSourceFace);
	}
	// A skeletal-mesh or cloth-asset garment: wrapped as an Outfit it gets the standard fit sizes like any Outfit.
	FMantleCosmeticDraft Wrapped;
	TArray<TStrongObjectPtr<UObject>> KeepAlive;
	TSharedPtr<void> Lease;
	FString Why, Route;
	if (FMantleParametricOutfit::PrepareWrapped(Draft, Contract, Wrapped, KeepAlive, Lease, Why, &Route))
	{
		FMantleCosmeticPackage Result = BuildPrepared(Wrapped, Contract, OutHairPreview, OutHairSourceFace);
		if (!Result.HasErrors()) { Result.Notes.Insert(FString::Printf(TEXT("Made as a MetaHuman Outfit (%s), so it uploads in Epic's standard body sizes."), *Route), 0); }
		return Result;
	}
	// One size, as the garment is: a cloth asset as its render geometry.
	FMantleCosmeticDraft Single = Draft;
	FString MeshError;
	USkeletalMesh* Mesh = FMantleParametricOutfit::GarmentMesh(Draft.Source.Get(), KeepAlive, MeshError);
	if (!Mesh)
	{
		FMantleCosmeticPackage Result;
		Result.Class = Draft.Class;
		Result.Slot = Draft.Slot;
		Result.Name = Draft.Name.TrimStartAndEnd();
		Result.Issues.Add(FMantleCosmeticChecks::Error(MeshError));
		return Result;
	}
	Single.Source = Mesh;
	FMantleCosmeticPackage Result = BuildPrepared(Single, Contract, OutHairPreview, OutHairSourceFace);
	Result.Notes.Insert(FString::Printf(TEXT("Uploads in one size: it could not be made in sizes (%s)."), *Why), 0);
	return Result;
}

FMantleCosmeticPackage FMantleCosmeticExport::BuildPrepared(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, FMantleSkinnedMesh* OutHairPreview, FMantleSkinnedMesh* OutHairSourceFace)
{
	using namespace MantleCosmeticExportPrivate;
	if (OutHairPreview) *OutHairPreview = FMantleSkinnedMesh();
	if (OutHairSourceFace) *OutHairSourceFace = FMantleSkinnedMesh();
	if (Draft.Class == EMantleCosmeticClass::Garment && !Draft.OutfitTargets.IsEmpty())
	{
		FMantleCosmeticPackage Result;
		Result.Class = Draft.Class;
		Result.Slot = Draft.Slot;
		Result.Name = Draft.Name;
		Result.Description = Draft.Description;
		if (!FMantleParametricOutfit::IsSource(Draft.Source.Get()) || Draft.OutfitTargets.Num() > 8)
		{
			Result.Issues.Add(FMantleCosmeticChecks::Error(TEXT("One logical native clothing piece needs one to eight representative source bodies.")));
			return Result;
		}
		TArray<FMantleOutfitSourceSize> SourceSizes;
		FString SourceError;
		if (!FMantleParametricOutfit::ListSizes(Draft.Source.Get(), SourceSizes, SourceError) || SourceSizes.IsEmpty())
		{
			Result.Issues.Add(FMantleCosmeticChecks::Error(SourceError)); return Result;
		}
		for (const FMantleOutfitSourceSize& Size : SourceSizes)
		{
			if (Size.Pieces.Num() != 1 || Size.OutfitGuids.Num() != 1 || SourceSizes[0].OutfitGuids.Num() != 1
				|| Size.OutfitGuids[0] != SourceSizes[0].OutfitGuids[0])
			{
				Result.Issues.Add(FMantleCosmeticChecks::Error(TEXT("One catalog clothing item requires the same single logical piece in every native source size."))); return Result;
			}
		}
		TArray<FMantleOutfitAuthoringTarget> Targets = Draft.OutfitTargets;
		Targets.Sort([](const FMantleOutfitAuthoringTarget& A, const FMantleOutfitAuthoringTarget& B) { return A.Id < B.Id; });
		TSet<FString> Seen;
		for (const FMantleOutfitAuthoringTarget& Target : Targets)
		{
			bool bValidId = Target.Id.Len() >= 1 && Target.Id.Len() <= 32 && Target.Id[0] >= TEXT('a') && Target.Id[0] <= TEXT('z');
			for (const TCHAR Character : Target.Id)
			{
				bValidId &= (Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_') || Character == TEXT('-');
			}
			if (!bValidId || Seen.Contains(Target.Id) || !Target.Character.IsValid() || !Target.Body.IsValid() || !Target.Face.IsValid())
			{
				Result.Issues.Add(FMantleCosmeticChecks::Error(TEXT("Every clothing source needs a unique lowercase ID, a Creator character, and its body and face.")));
				return Result;
			}
			Seen.Add(Target.Id);
			FMantleCosmeticDraft Input = Draft;
			Input.OutfitTargets.Reset();
			Input.OutfitSourceSize.Reset(); // Native selection chooses from every authored size for this target.
			Input.OutfitAuthoringCharacter = Target.Character;
			Input.bOutfitAuthoringInMemory = Target.bInMemory;
			Input.AuthoringBody = Target.Body;
			Input.AuthoringFace = Target.Face;
			FMantleCosmeticDraft Prepared;
			TArray<TStrongObjectPtr<UObject>> KeepAlive;
			FString Error;
			if (!FMantleParametricOutfit::Prepare(Input, Contract, Prepared, KeepAlive, Error))
			{
				Result.Issues.Add(FMantleCosmeticChecks::Error(Target.Id + TEXT(": ") + Error)); return Result;
			}
			FMantleCosmeticPackage Part = BuildPrepared(Prepared, Contract);
			if (Part.HasErrors()) { Result.Issues.Append(Part.Issues); return Result; }
			if (!Result.GarmentVariants.IsEmpty() && Part.Materials.Num() == Result.Materials.Num())
			{
				// Areas are found on each source's own texels, so a later size can find a small extra area or miss one.
				// The sections and their masks are the garment's own on every source: each takes the first source's areas.
				for (int32 Index = 0; Index < Part.Materials.Num(); ++Index)
				{
					FMantleCosmeticMaterialRef& Material = Part.Materials[Index];
					const FMantleCosmeticMaterialRef& First = Result.Materials[Index];
					if (First.ColorRegions.IsEmpty())
					{
						Material.ColorRegions.Reset();
						Material.ColorMaskKey.Reset();
					}
					else if (!Material.ColorMaskKey.IsEmpty())
					{
						Material.ColorRegions = First.ColorRegions;
					}
				}
			}
			if (!Result.GarmentVariants.IsEmpty())
			{
				// A found area's default colour is its average over this source's texels; one logical control keeps
				// the first source's default (and name), so the variants declare the same controls.
				for (FMantleCosmeticMaterialRef& Material : Part.Materials)
				{
					for (FMantleClothingColorRegion& Region : Material.ColorRegions)
					{
						for (const FMantleCosmeticMaterialRef& First : Result.Materials)
						{
							if (const FMantleClothingColorRegion* Same = First.ColorRegions.FindByPredicate([&Region](const FMantleClothingColorRegion& Entry) { return Entry.Id == Region.Id; }))
							{
								Region.DefaultColor = Same->DefaultColor; Region.Label = Same->Label; break;
							}
						}
					}
				}
			}
			if (!Result.GarmentVariants.IsEmpty())
			{
				// A grouped catalog item has the default target's thumbnail. Variants do
				// not reference thumbnails, so do not upload later targets' unused images.
				const FString UnusedThumbnail = Part.ThumbnailKey;
				Part.Assets.RemoveAll([&UnusedThumbnail](const FMantleCosmeticUploadAsset& Asset) { return Asset.Key == UnusedThumbnail; });
				Part.ThumbnailKey.Reset();
			}
			const FString Prefix = TEXT("variant/") + Target.Id + TEXT("/");
			auto NamespaceKey = [&Prefix](FString& Key) { if (!Key.IsEmpty()) { Key = Prefix + Key; } };
			NamespaceKey(Part.ThumbnailKey);
			NamespaceKey(Part.MeshKey);
			NamespaceKey(Part.SourceShapeKey);
			NamespaceKey(Part.FaceMeshKey);
			NamespaceKey(Part.CoverageKey);
			for (FMantleCosmeticMaterialRef& Material : Part.Materials)
			{
				NamespaceKey(Material.AlbedoKey); NamespaceKey(Material.NormalKey); NamespaceKey(Material.OrmKey); NamespaceKey(Material.ColorMaskKey);
			}
			for (FMantleCosmeticUploadAsset& Asset : Part.Assets) { NamespaceKey(Asset.Key); NamespaceKey(Asset.SourceKey); }
			FMantleCosmeticGarmentVariantExport Variant;
			Variant.Id = Target.Id;
			// A label only (the catalog keeps at most 64 characters): a seller's size can be named after its whole mesh path.
			Variant.NativeSourceSize = Prepared.OutfitSourceSize.Left(64);
			Variant.MeshKey = Part.MeshKey;
			Variant.SourceShapeKey = Part.SourceShapeKey;
			Variant.FaceMeshKey = Part.FaceMeshKey;
			Variant.CoverageKey = Part.CoverageKey;
			Variant.Materials = Part.Materials;
			if (Result.GarmentVariants.IsEmpty()) { Result = MoveTemp(Part); Result.DefaultGarmentVariant = Target.Id; }
			else { Result.Assets.Append(MoveTemp(Part.Assets)); Result.Issues.Append(Part.Issues); Result.Notes.Append(Part.Notes); }
			Result.GarmentVariants.Add(MoveTemp(Variant));
			Result.Notes.Add(FString::Printf(TEXT("Clothing source %s: native auto-selected %s; one catalog item, runtime policy body-rms-v1."), *Target.Id, *Prepared.OutfitSourceSize));
		}
		if (!Draft.FitBodiesStatus.IsEmpty()) { Result.Notes.Insert(Draft.FitBodyNotes, 0); Result.Notes.Insert(Draft.FitBodiesStatus, 0); }
		return Result;
	}
	if (Draft.Class == EMantleCosmeticClass::Accessory && FMantleParametricOutfit::IsSource(Draft.Source.Get()))
	{
		// A hat (or any rigid piece) from a MetaHuman Outfit: one rigid mesh on the reference head, fitted per avatar by its socket.
		FMantleCosmeticDraft Prepared;
		TArray<TStrongObjectPtr<UObject>> KeepAlive;
		FString Error, RigidNote;
		if (FMantleParametricOutfit::PrepareRigid(Draft, Contract, Prepared, KeepAlive, Error, &RigidNote))
		{
			FMantleCosmeticPackage Result = BuildPrepared(Prepared, Contract);
			Result.Notes.Add(FString::Printf(TEXT("Outfit source size: %s, as a rigid accessory."), *Prepared.OutfitSourceSize));
			if (!RigidNote.IsEmpty()) { Result.Notes.Add(RigidNote); }
			return Result;
		}
		FMantleCosmeticPackage Result;
		Result.Class = Draft.Class;
		Result.Slot = Draft.Slot;
		Result.Name = Draft.Name.TrimStartAndEnd();
		Result.Description = Draft.Description.TrimStartAndEnd();
		Result.Issues.Add(FMantleCosmeticChecks::Error(Error));
		return Result;
	}
	if (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()))
	{
		FMantleCosmeticDraft Prepared;
		TArray<TStrongObjectPtr<UObject>> KeepAlive;
		FString Error, ReducedNote;
		if (FMantleParametricOutfit::Prepare(Draft, Contract, Prepared, KeepAlive, Error, &ReducedNote))
		{
			FMantleCosmeticPackage Result = BuildPrepared(Prepared, Contract);
			Result.Notes.Add(FString::Printf(TEXT("Outfit source size: %s; exported render geometry uses Mantle's existing runtime fit."), *Prepared.OutfitSourceSize));
			if (!ReducedNote.IsEmpty()) { Result.Notes.Add(ReducedNote); }
			return Result;
		}
		FMantleCosmeticPackage Result;
		Result.Class = Draft.Class;
		Result.Slot = Draft.Slot;
		Result.Name = Draft.Name.TrimStartAndEnd();
		Result.Description = Draft.Description.TrimStartAndEnd();
		Result.Issues.Add(FMantleCosmeticChecks::Error(Error));
		return Result;
	}

	FMantleCosmeticPackage Package;
	Package.Class = Draft.Class;
	Package.AvatarPublicationMode = Draft.AvatarPublicationMode;
	Package.Visibility = Draft.bPublic ? TEXT("public") : TEXT("private");
	Package.RightsDeclarationVersion = Draft.bPublic && Draft.bRightsDeclared ? FString(MantleRights::DeclarationVersion) : FString();
	if (Draft.Class != EMantleCosmeticClass::Garment && !Draft.ClothingColors.IsEmpty())
	{
		Package.Issues.Add(FMantleCosmeticChecks::Error(TEXT("Declared clothing colors are supported only on clothing items.")));
		return Package;
	}
	Package.Slot = Draft.Slot;
	Package.Name = Draft.Name.TrimStartAndEnd();
	Package.Description = Draft.Description.TrimStartAndEnd();
	TArray<FMantleCosmeticIssue>& Issues = Package.Issues;

	FMantleCosmeticChecks::CheckNameAndDescription(Draft.Name, Draft.Description, Issues);

	// An existing item target publishes a new version into it, for any class; the API checks ownership and that the slot matches.
	Package.TargetItemId = Draft.PublishIntoItemId.TrimStartAndEnd();

	if (Draft.Class == EMantleCosmeticClass::Overlay)
	{
		BuildOverlayPackage(Draft, Contract, Package);
		return Package;
	}

	const FMantleContractSlot* Slot = Contract.FindSlot(Draft.Slot);
	if (!Slot || Slot->ItemClass != ContractItemClass(Draft.Class)
		|| (Draft.Class == EMantleCosmeticClass::Hair) != Contract.HairSlots.Contains(Draft.Slot))
	{
		Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("\"%s\" is not a slot for %s."), *Draft.Slot, LexToString(Draft.Class)),
			TEXT("Choose a slot from the list.")));
		return Package;
	}

	UObject* Source = Draft.Source.Get();
	if (!Source)
	{
		Issues.Add(FMantleCosmeticChecks::Error(TEXT("choose the asset to upload.")));
		return Package;
	}

	FString Error;
	UObject* ThumbnailOf = Source;
	USkeletalMesh* ThumbnailBody = nullptr;
	// A garment split from an outfit is framed on its kept sections; invalid frames the whole mesh.
	FBox ThumbnailFrame(ForceInit);

	switch (Draft.Class)
	{
	case EMantleCosmeticClass::Hair:
	case EMantleCosmeticClass::Garment:
		{
			const bool bHair = Draft.Class == EMantleCosmeticClass::Hair;
			FMantleContractClass Class = Contract.SkinnedGarment;
			Class.MaxTriangles = Contract.SkinnedMaxTriangles(Draft.Slot);
			USkeletalMesh* Body = Draft.AuthoringBody.Get();
			if (!Body)
			{
				Body = FindAuthoringBody(Source, Contract, !bHair);
				if (Body)
				{
					Package.Notes.Add(FString::Printf(TEXT("Authored on %s (found in the project)"), *Body->GetPathName()));
				}
			}
			if (!Body)
			{
				FString HeadError;
				if (bHair) FMantleHairSourceDiscovery::FindHead(Source,HeadError);
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("choose the MetaHuman body this was made on (Authored on)."),
					bHair ? HeadError + TEXT(". Choose the actual paired body or head rig; its head bind pose locates the cards.") :
					TEXT("It is uploaded with the item so the item fits every body; any MetaHuman body exported at Medium, High or Cinematic quality works, including a Cinematic MetaHuman's SKM_<Name>_Body under CharacterAssemblies/<Name>/Body.")));
				return Package;
			}
			ThumbnailBody = Body;

			FMeshSource Mesh;
			// A garment skinned to root alone takes this body's skin (every LOD).
			FMantleGlbMesh BodySkin;
			bool bHairCards = false;
			TArray<UTexture2D*> CardTextures;
			TArray<TArray<uint8>> GroomAtlases;
			TArray<FString> GroomNativeProfiles;
			TArray<TArray<uint8>> GroomTangents;
			TArray<FMantleNativeHairSettings> GroomNativeSettings;
			TArray<TArray<UMaterialInterface*>> GroomColorSources;
			// The materials a groom's cards take their colour from: the groom's, then the cards mesh's.
			TArray<UMaterialInterface*> HairColorSources;
			// Hair always takes LOD 0 of its source; a garment the first LOD within the limit.
			int32 SourceLODs = 1;
			EMantleLODPick LODPick = EMantleLODPick::LODZeroOnly;
			if (!bHair)
			{
				USkeletalMesh* Garment = Cast<USkeletalMesh>(Source);
				if (!Garment)
				{
					Issues.Add(FMantleCosmeticChecks::Error(TEXT("clothing uploads a skeletal mesh skinned to the MetaHuman body.")));
					return Package;
				}
				SourceLODs = UMantleBodyTopologyData::GetNumLODs(Garment);
				LODPick = EMantleLODPick::FirstWithinLimit;
				// Only the chosen sections upload; the LOD, the limits, coverage and bind pose are judged on them.
				const TArray<int32>* KeepSlots = Draft.KeepSlots.GetPtrOrNull();
				if (KeepSlots)
				{
					if (KeepSlots->Num() == 0)
					{
						Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("no section of %s is chosen for this item."), *Garment->GetName()),
							FString::Printf(TEXT("Tick the sections that make up the %s under Sections."), *Draft.Slot)));
						return Package;
					}
					TArray<int32> MeshSlots;
					const TArray<FMantleOutfitSection> All = ReadOutfitSections(Garment, 0, MeshSlots);
					TArray<FString> Kept;
					TArray<FString> Left;
					for (int32 Index = 0; Index < All.Num(); ++Index)
					{
						(KeepSlots->Contains(MeshSlots[Index]) ? Kept : Left).Add(All[Index].SlotName);
					}
					Package.Notes.Add(FString::Printf(TEXT("Sections uploaded as the %s: %s (left out: %s)"), *Draft.Slot,
						Kept.Num() > 0 ? *FString::Join(Kept, TEXT(", ")) : TEXT("none"), Left.Num() > 0 ? *FString::Join(Left, TEXT(", ")) : TEXT("none")));
				}
				const int32 LOD = ChooseLODWithin(Garment, Class.MaxTriangles, KeepSlots);
				if (LOD > 0)
				{
					Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(TEXT("LOD %d is uploaded: LOD 0 has %d triangles, the limit is %d."),
						LOD, CountTriangles(Garment, 0, KeepSlots), Class.MaxTriangles)));
				}
				if (!ReadSkeletalLOD(Garment, LOD, Mesh, Error, KeepSlots))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Error));
					return Package;
				}
				if (KeepSlots)
				{
					// The thumbnail frames the kept piece, not the whole outfit.
					ThumbnailFrame = FBox(Mesh.Bounds);
				}
				int32 Dropped = 0;
				if (!FitToClassSkeleton(Mesh, Class, Contract, false, Dropped, Error))
				{
					Issues.Add(FMantleCosmeticChecks::Error(FMantleCosmeticChecks::EndWithFullStop(Error), TEXT("Clothing must be skinned to the MetaHuman body skeleton (metahuman_base_skel).")));
					return Package;
				}
				// The folded bones are in Dropped too; they get their own note (NoteFoldedBones).
				const int32 Helpers = Dropped - Mesh.FoldedBones.Num();
				if (Helpers > 0)
				{
					Package.Notes.Add(FString::Printf(TEXT("%d helper bones that carry no weight and are not on the MetaHuman skeleton were left out"), Helpers));
				}
				// Weights on root alone (a cloth asset or static mesh made into an Outfit) cannot follow the body: the client
				// takes the wearer's weights by bone name. The garment takes its authoring body's skin and bones instead.
				if (FMantleGarmentOutfit::FollowsRootOnly(Mesh.Glb))
				{
					int32 Far = 0;
					double MeanCm = 0.0;
					if (!ReadBodySkin(Body, Class, Contract, BodySkin, Error) || !FMantleGarmentOutfit::TransferWeights(Mesh.Glb, BodySkin, Far, MeanCm, Error))
					{
						Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is skinned to its root bone only, so it would not move with the body (%s)."),
							*Garment->GetName(), *Error),
							TEXT("Skin the clothing to the MetaHuman body skeleton (metahuman_base_skel), or choose the MetaHuman body it was made on under Authored on.")));
						return Package;
					}
					Mesh.MostInfluences = Mesh.Glb.NumInfluences;
					Package.Notes.Add(FString::Printf(TEXT("Skinned to its root bone only: took the skin of %s (%d bones; %d of %d vertices more than %.0f cm from it, mean %.2f cm)"),
						*Body->GetName(), Mesh.Glb.Joints.Num(), Far, Mesh.Glb.Positions.Num(), FMantleGarmentOutfit::NearCm, MeanCm));
				}

				// Skin sections and extra garment pieces among the sections that upload (all of them unless the panel
				// kept some).
				TArray<FMantleOutfitSection> OutfitSections;
				for (int32 Index = 0; Index < Mesh.Glb.MaterialNames.Num(); ++Index)
				{
					FMantleOutfitSection& Section = OutfitSections.AddDefaulted_GetRef();
					Section.SlotName = Mesh.Glb.MaterialNames[Index];
					UMaterialInterface* Material = Mesh.Materials.IsValidIndex(Index) ? Mesh.Materials[Index] : nullptr;
					const UMaterial* BaseMaterial = Material ? Material->GetBaseMaterial() : nullptr;
					Section.MaterialName = Material ? Material->GetName() : FString();
					Section.BaseMaterialName = BaseMaterial ? BaseMaterial->GetName() : FString();
				}
				FMantleOutfitSections::CheckGarmentSections(OutfitSections, Draft.Slot, Issues);
			}
			else if (IsGroomAsset(Source))
			{
				const TArray<FMantleGroomCardsEntry> Entries = ReadGroomCards(Source);
				int32 Chosen = Entries.IsValidIndex(Draft.CardsEntry) ? Draft.CardsEntry : DefaultCardsEntry(Entries);
				if (!Entries.IsValidIndex(Chosen) || !Entries[Chosen].Mesh.IsValid())
				{
					Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s has no hair cards mesh."), *Source->GetName()),
						TEXT("Use Generate cards from strands (optional installed Hair Card Generator), or make a MetaHuman Optimized/Medium assembly with cards, then select its Groom or standalone cards mesh.")));
					return Package;
				}
				TSet<int32> Groups;
				for (const auto& E : Entries) if (E.Mesh.IsValid()) Groups.Add(E.GroupIndex);
				TArray<int32> GroupOrder = Groups.Array(); GroupOrder.Sort();
				const UGroomAsset* NativeGroom = Cast<UGroomAsset>(Source);
				if (!NativeGroom || GroupOrder.IsEmpty())
				{ Issues.Add(FMantleCosmeticChecks::Error(TEXT("native v2 needs a groom with at least one cards group"), TEXT("Export standalone artist cards, or use a groom with hair cards."))); return Package; }
				// Each physical cards group is its own section with its own atlas, tangent and colour. Groups with strands but
				// no cards (MetaHuman's ponytail tie, for one) have nothing to draw at cards LODs and stay out.
				for (int32 Group = 0; Group < NativeGroom->GetHairGroupsInfo().Num(); ++Group)
				{
					if (!Groups.Contains(Group)) Package.Notes.Add(FString::Printf(TEXT("Hair group %d has no cards and is left out"), Group));
				}
				// The entry for a group at a groom cards LOD: the nearest authored LOD of that group.
				auto PickForGroup = [&Entries](int32 Group, int32 WantedLOD)
				{
					const FMantleGroomCardsEntry* Found = nullptr;
					for (const auto& E : Entries)
					{
						if (E.GroupIndex != Group || !E.Mesh.IsValid()) continue;
						if (!Found || FMath::Abs(E.LODIndex - WantedLOD) < FMath::Abs(Found->LODIndex - WantedLOD)) Found = &E;
					}
					return Found;
				};
				// Unless the creator chose an entry, take the finest cards LOD whose groups together fit the budget: a finer
				// authored LOD beats LOD 0 with whole cards deleted (bald patches).
				if (!Entries.IsValidIndex(Draft.CardsEntry))
				{
					TArray<int32> LODValues;
					for (const auto& E : Entries) if (E.Mesh.IsValid()) LODValues.AddUnique(E.LODIndex);
					LODValues.Sort();
					TArray<TArray<int32>> PerLOD;
					for (const int32 LODValue : LODValues)
					{
						TArray<int32>& Counts = PerLOD.AddDefaulted_GetRef();
						for (int32 Group : GroupOrder)
						{
							const FMantleGroomCardsEntry* Found = PickForGroup(Group, LODValue);
							int32 Triangles = 0;
							if (const FStaticMeshRenderData* Render = Found ? Found->Mesh->GetRenderData() : nullptr)
							{
								if (Render->LODResources.Num() > 0) for (const auto& Section : Render->LODResources[0].Sections) Triangles += Section.NumTriangles;
							}
							Counts.Add(Triangles);
						}
					}
					const int32 Picked = ChooseGroomCardsLOD(PerLOD, Class.MaxTriangles);
					if (LODValues.IsValidIndex(Picked))
					{
						int32 Total = 0; for (const int32 Count : PerLOD[Picked]) Total += Count;
						for (const auto& E : Entries) if (E.Mesh.IsValid() && E.LODIndex == LODValues[Picked]) { Chosen = E.Index; break; }
						Package.GroomCardsLOD = LODValues[Picked];
						if (Class.MaxTriangles > 0 && Total > Class.MaxTriangles)
						{
							Package.Notes.Add(FString::Printf(TEXT("Hair cards: using the groom's cards LOD %d, %d triangles trimmed to %d (budget %d)"), LODValues[Picked], Total, Class.MaxTriangles, Class.MaxTriangles));
						}
						else
						{
							Package.Notes.Add(FString::Printf(TEXT("Hair cards: using the groom's cards LOD %d, %d triangles (budget %d)"), LODValues[Picked], Total, Class.MaxTriangles));
						}
					}
				}
				else
				{
					Package.GroomCardsLOD = Entries[Chosen].LODIndex;
				}
				for (int32 Group : GroupOrder)
				{
					const FMantleGroomCardsEntry* Pick = PickForGroup(Group, Entries[Chosen].LODIndex);
					if (!Pick) continue;
					FString NativeProfileDescription;
					FString NativeProfile;
					FMantleNativeHairSettings NativeSettings;
					// The signed tangent (RGB*2-1, HairCardsAttributeCommon.ush GetTangent) is texture 2 in the default layouts and
					// texture 0 in the compact ones MetaHuman Creator's grooms use; its encoding is the same in every layout.
					const int32 TangentSlot = Pick->TextureLayout <= 1 ? 2 : 0;
					if (Pick->TextureLayout < 0 || Pick->TextureLayout > 3 || !Pick->Textures.IsValidIndex(TangentSlot) || !Pick->Textures[TangentSlot].IsValid() || Pick->Textures[TangentSlot]->SRGB)
					{ Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("native v2 needs the groom's linear RGB tangent atlas (cards group %d, layout %d, slot %d)"), Group, Pick->TextureLayout, TangentSlot))); return Package; }
                    if (!FMantleNativeHairProfile::FromMaterialRecipe(Pick->EffectiveCardsMaterial.Get(), NativeProfile, NativeSettings, NativeProfileDescription, Error))
					{
						Issues.Add(FMantleCosmeticChecks::Error(Error, TEXT("Select the native cards material slot and supported MetaHuman profile, or export standalone cards with your own albedo/alpha material for a custom opacity curve.")));
						return Package;
					}
					Package.Notes.Add(NativeProfileDescription);
					FMeshSource Part;
					if (!ReadStaticLOD(Pick->Mesh.Get(), 0, Part, Error)) { Issues.Add(FMantleCosmeticChecks::Error(Error)); return Package; }
					// The editor's thumbnail of a groom asset is an empty frame; the first group's cards show the style.
					if (ThumbnailOf == Source) ThumbnailOf = Pick->Mesh.Get();
					TArray<uint8> Atlas;
					FString AtlasDescription;
					if (!NormalizeGroomAtlas(*Pick, Contract.HairMaxTextureSize, Atlas, AtlasDescription, Error)) { Issues.Add(FMantleCosmeticChecks::Error(Error, TEXT("Provide the groom's complete cards atlas slots, or upload standalone cards with albedo/opacity materials."))); return Package; }
					Package.Notes.Add(AtlasDescription);
                    TArray<FColor> TangentPixels; FIntPoint TangentSize;
                    if (!ReadTexturePixels(Pick->Textures[TangentSlot].Get(), Contract.HairMaxTextureSize, true, TangentPixels, TangentSize, Error))
                    { Issues.Add(FMantleCosmeticChecks::Error(Error)); return Package; }
                    // Signed direction is encoded in RGB. No BC5 conversion, reconstructed Z or vector normalization.
                    for (FColor& Pixel : TangentPixels) Pixel.A = 255;
                    TArray<uint8> TangentPng = FMantlePng::EncodeRGBA8(TangentSize.X, TangentSize.Y, TangentPixels);
                    TArray<FColor> TangentRoundTrip; FIntPoint TangentRoundTripSize;
                    if (!DecodePngPixels(TangentPng, Contract.HairMaxTextureSize, true, TangentRoundTrip, TangentRoundTripSize)
                        || TangentRoundTripSize != TangentSize || TangentRoundTrip != TangentPixels)
                    { Issues.Add(FMantleCosmeticChecks::Error(TEXT("native v2 tangent PNG did not retain exact linear RGB"))); return Package; }
					if (Pick->bInvertUV) for (auto& UV : Part.Glb.UVs) UV.Y = 1.f - UV.Y;
					const int32 VertexBase = Mesh.Glb.Positions.Num(), MaterialBase = Mesh.Glb.MaterialNames.Num();
					Mesh.Glb.Name = Source->GetName(); Mesh.Glb.Positions.Append(Part.Glb.Positions); Mesh.Glb.Normals.Append(Part.Glb.Normals); Mesh.Glb.Tangents.Append(Part.Glb.Tangents); Mesh.Glb.UVs.Append(Part.Glb.UVs);
					Mesh.Bounds += Part.Bounds; Mesh.NumTriangles += Part.NumTriangles;
					TArray<UMaterialInterface*> Colors; for (const auto& M : Pick->Materials) if (M.IsValid()) Colors.Add(M.Get());
					for (int32 M = 0; M < Part.Glb.MaterialNames.Num(); ++M)
					{
						Mesh.Glb.MaterialNames.Add(FString::Printf(TEXT("Group%d_%s"), Group, *Part.Glb.MaterialNames[M]));
						Mesh.Materials.Add(Part.Materials.IsValidIndex(M) ? Part.Materials[M] : nullptr);
						GroomAtlases.Add(Atlas); GroomColorSources.Add(Colors); GroomNativeProfiles.Add(NativeProfile);
						GroomTangents.Add(TangentPng); GroomNativeSettings.Add(NativeSettings);
					}
					for (auto Primitive : Part.Glb.Primitives) { Primitive.FirstVertex += VertexBase; Primitive.MaterialIndex += MaterialBase; Mesh.Glb.Primitives.Add(MoveTemp(Primitive)); }
					Package.Notes.Add(FString::Printf(TEXT("Hair cards: %s, atlas layout %d normalized to coverage/depth/seed"), *Pick->Describe(), Pick->TextureLayout));
				}
				bHairCards = true;
			}
			else if (UStaticMesh* Cards = Cast<UStaticMesh>(Source))
			{
				int32 CardsLOD = 0;
				if (const auto* Render = Cards->GetRenderData())
				{
					SourceLODs = Render->LODResources.Num();
					for (int32 LOD = 0; LOD < SourceLODs; ++LOD) { int32 Triangles = 0; for (const auto& Section : Render->LODResources[LOD].Sections) Triangles += Section.NumTriangles; if (Triangles > 0 && Triangles <= Class.MaxTriangles) { CardsLOD = LOD; break; } }
				}
				if (!ReadStaticLOD(Cards, CardsLOD, Mesh, Error))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Error));
					return Package;
				}
				TArray<FString> AtlasLinks;
				CardTextures = FindCardAtlasesNear(Cards, AtlasLinks);
				for (int32 Index = 0; Index < CardTextures.Num(); ++Index)
				{
					Package.Notes.Add(FString::Printf(TEXT("Hair atlas of the cards: %s (tied to them by %s)"), *CardTextures[Index]->GetPathName(),
						AtlasLinks.IsValidIndex(Index) ? *AtlasLinks[Index] : TEXT("its name")));
				}
				if (CardTextures.Num() == 0)
				{
					const FString Stem = CardsGroomStem(Cards->GetName());
					Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("no hair cards atlas belonging to %s was found (none of its materials or a groom named %s references one, and no texture near it is named %s_CardsAtlas_Attribute), so its material's textures are used."),
						*Cards->GetName(), *Stem, *Stem),
						FString::Printf(TEXT("Keep the groom %s next to its cards mesh, or its CardsAtlas textures named after it (%s_CardsAtlas_Attribute, %s_CardsAtlas_Tangent), or upload the groom itself."), *Stem, *Stem, *Stem)));
				}
				bHairCards = true;
			}
			else if (USkeletalMesh* Skinned = Cast<USkeletalMesh>(Source))
			{
				if (Draft.SourceSection == INDEX_NONE && Skinned == FindAuthoringFace(Draft,Body))
				{
					Issues.Add(FMantleCosmeticChecks::Error(TEXT("a whole authoring face cannot be uploaded as hair"),
						TEXT("Choose the eyelash, brow or hair section to lift from the face, or select a standalone hair cards mesh.")));
					return Package;
				}
				// Whole skeletal card meshes are valid. For a lifted section, keep its material slot across authored LODs.
				SourceLODs = UMantleBodyTopologyData::GetNumLODs(Skinned);
				TArray<int32> Slots;
				if (Draft.SourceSection != INDEX_NONE)
				{
					const auto Spans = ReadSectionSpans(Skinned, 0);
					if (!Spans.IsValidIndex(Draft.SourceSection)) { Issues.Add(FMantleCosmeticChecks::Error(TEXT("the selected hair section is missing"))); return Package; }
					Slots.Add(Spans[Draft.SourceSection].Slot);
				}
				for (int32 LOD = 0; LOD < SourceLODs; ++LOD)
				{
					FMeshSource Candidate;
					if (!ReadSkeletalLOD(Skinned, LOD, Candidate, Error, Slots.IsEmpty() ? nullptr : &Slots)) continue;
					if (LOD == 0 || Candidate.NumTriangles <= Class.MaxTriangles) Mesh = MoveTemp(Candidate);
					if (Mesh.NumTriangles <= Class.MaxTriangles) break;
				}
				if (Mesh.Glb.Positions.IsEmpty()) { Issues.Add(FMantleCosmeticChecks::Error(Error)); return Package; }
				bHairCards = true;
				Package.Notes.Add(FString::Printf(TEXT("Skeletal hair: authored LOD %d, %d triangles; face hair bones fold to head for transport"), Mesh.LODIndex, Mesh.NumTriangles));
			}
			else
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("hair uploads a groom, a hair cards static mesh, or one section of a skeletal mesh.")));
				return Package;
			}

			if (bHair)
			{
				// Which route this hair takes, in words for the panel; sculpted solid hair is flagged, not blocked.
				{
					TArray<uint32> RouteIndices;
					for (const FMantleGlbPrimitive& Primitive : Mesh.Glb.Primitives)
					{
						for (const uint32 Index : Primitive.Indices) RouteIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
					}
					FMantleHairRoute::FInput RouteInput;
					RouteInput.bGroom = IsGroomAsset(Source);
					RouteInput.bSkeletalMesh = Source && Source->IsA<USkeletalMesh>();
					RouteInput.bMetaHumanOrigin = !RouteInput.bGroom && !RouteInput.bSkeletalMesh && !ShouldFitThirdPartyHairCardsToHead(Source);
					RouteInput.bPlacedByHand = Draft.bPlaceHairByHand;
					RouteInput.Triangles = Mesh.NumTriangles;
					RouteInput.Budget = Class.MaxTriangles;
					if (!RouteInput.bGroom && !RouteInput.bSkeletalMesh)
					{
						for (const TArray<int32>& Piece : FMantleHairCards::FindCards(Mesh.Glb.Positions, RouteIndices))
						{
							++RouteInput.Pieces;
							RouteInput.LargestPieceTriangles = FMath::Max(RouteInput.LargestPieceTriangles, Piece.Num());
						}
					}
					Package.HairRoute = FMantleHairRoute::Decide(RouteInput);
					Package.bHasHairRoute = true;
					Package.Notes.Add(FString::Printf(TEXT("Hair route: %s. %s"), *Package.HairRoute.Title, *Package.HairRoute.Words));
					if (!Package.HairRoute.bSupported)
					{
						Issues.Add(FMantleCosmeticChecks::Warning(Package.HairRoute.Words, TEXT("Hair cards, MetaHuman grooms and skeletal hair are supported today.")));
					}
				}
				const int32 Before = Mesh.NumTriangles;
				TArray<FString> BudgetWarnings;
				if (!ReduceHairCards(Mesh, Class.MaxTriangles, Error, &BudgetWarnings)) { Issues.Add(FMantleCosmeticChecks::Error(Error)); return Package; }
				for (const FString& Warning : BudgetWarnings) Issues.Add(FMantleCosmeticChecks::Warning(Warning));
				if (Before != Mesh.NumTriangles) Package.Notes.Add(FString::Printf(TEXT("Whole-card budget reduction: %d to %d triangles (no card collapses)"), Before, Mesh.NumTriangles));
				if (!BindRigidlyToHead(Mesh, Body, Error))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Error));
					return Package;
				}
				if (bHairCards)
				{
					// The hair card stream (flag bit 4): CoordU from the cards' geometry, as the
					// engine's groom builder computes it. Root UVs are written as zero: the groom's strand
					// data is not read.
					TArray<uint32> AllIndices;
					for (const FMantleGlbPrimitive& Primitive : Mesh.Glb.Primitives)
					{
						for (const uint32 Index : Primitive.Indices)
						{
							AllIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
						}
					}
					int32 CardCount = 0;
					bool bMainU = true;
					Mesh.Glb.HairCoordU = FMantleHairCards::ComputeCoordU(Mesh.Glb.Positions, Mesh.Glb.UVs, AllIndices, &CardCount, &bMainU);
					Mesh.Glb.HairRootUV.Init(FVector2f::ZeroVector, Mesh.Glb.Positions.Num());
					Package.Notes.Add(FString::Printf(TEXT("Hair card stream: %d cards, strands run along %s"), CardCount, bMainU ? TEXT("U") : TEXT("V")));
				}
			}

			NoteFoldedBones(Mesh, FString(), Issues);
			NoteCappedInfluences(Mesh, FString(), Issues);
			FMantleCosmeticChecks::CheckMeshLimits(Mesh.NumTriangles, Mesh.Glb.Positions.Num(), Mesh.Glb.MaterialNames.Num(), Mesh.MostInfluences, Class, Issues,
				SourceLODs, LODPick);
			FMantleCosmeticChecks::CheckSectionNames(Mesh.Glb.MaterialNames, Issues);
			CheckSectionUVTiles(Mesh, TEXT("section"), Issues);
			if (FMantleCosmeticChecks::HasErrors(Issues))
			{
				return Package;
			}

			// The face the body goes with. The body shape carries the neck measured on it (garments and hair
			// share one shape per body and face), and a garment uploads it as the face it was authored with, so
			// players' clients fit the collar around each wearer's neck. Optional: without one the collar is
			// fitted from the body alone, as before, so a face that cannot be read is a warning, never an error.
			// Nails (#740) sit on the fingertips and have no collar: they upload without a face. A client takes a non-body
			// wear piece that names a face for head-bound hair, and fitting nails to the head fails, so they never show.
			const bool bFaceless = FMantleAvatarRefit::IsOverlaySlot(Draft.Slot);
			USkeletalMesh* AuthoringFace = bFaceless ? nullptr : FindAuthoringFace(Draft, Body);
			FMeshSource FaceMesh;
			FMantleSkinnedMeshLOD FaceSurface;
			bool bFace = false;
			bool bGeneratedHeadSource = false;
			if (AuthoringFace)
			{
				FString FaceError;
				TArray<FMantleCosmeticIssue> FaceIssues;
				FString FaceNote;
				if (ReadFaceSource(AuthoringFace, Contract, FaceMesh, FaceError, bHair, &bGeneratedHeadSource, &FaceNote))
				{
					FMantleCosmeticChecks::CheckMeshLimits(FaceMesh.NumTriangles, FaceMesh.Glb.Positions.Num(), FaceMesh.Glb.MaterialNames.Num(),
						FaceMesh.MostInfluences, Contract.AvatarFace, FaceIssues, UMantleBodyTopologyData::GetNumLODs(AuthoringFace), EMantleLODPick::FirstWithinLimit);
					FMantleCosmeticChecks::CheckSectionNames(FaceMesh.Glb.MaterialNames, FaceIssues);
					for (const FMantleCosmeticIssue& FaceIssue : FaceIssues)
					{
						if (FaceIssue.Severity == EMantleCosmeticSeverity::Error && FaceError.IsEmpty())
						{
							FaceError = FaceIssue.Message;
						}
					}
				}
				bFace = FaceError.IsEmpty();
				if (bFace)
				{
					if (!FaceNote.IsEmpty()) { Package.Notes.Add(FaceNote); }
					if (bGeneratedHeadSource) Package.Notes.Add(FString::Printf(TEXT("Head binding surface normalized from the same source %s LOD%d to %d triangles on a transient duplicate; source UV sections and reference rig retained, source asset unchanged."),*AuthoringFace->GetPathName(),FaceMesh.LODIndex,FaceMesh.NumTriangles));
					FaceSurface = UploadedFaceSurface(FaceMesh);
					Package.Notes.Add(FString::Printf(TEXT("Authored with the face %s (LOD %d, %d triangles%s)"), *AuthoringFace->GetPathName(), FaceMesh.LODIndex,
						FaceMesh.NumTriangles, Draft.AuthoringFace.IsValid() ? TEXT("") : TEXT(", found next to the body")));
					if (!bHair)
					{
						// A garment uploads this face (its faceMeshAssetId), with its weights as capped.
						NoteFoldedBones(FaceMesh, FString::Printf(TEXT("the face %s"), *AuthoringFace->GetName()), Issues);
						NoteCappedInfluences(FaceMesh, FString::Printf(TEXT("the face %s"), *AuthoringFace->GetName()), Issues);
					}
				}
				else
				{
					if (bHair)
					{
						Issues.Add(FMantleCosmeticChecks::Error(
							FString::Printf(TEXT("the authoring face %s cannot be used for hair head fitting (%s)"),*AuthoringFace->GetName(),*FaceError),
							TEXT("Provide a LOD of this same authoring head within the face limits, preserving its skin UVs, bone poses and weights.")));
						return Package;
					}
					Issues.Add(FMantleCosmeticChecks::Warning(
						FString::Printf(TEXT("the face %s cannot go with it (%s), so collars are fitted from the body alone."), *AuthoringFace->GetName(), *FaceError),
						TEXT("It can still be published; collars may sit off the neck of much thinner or heavier wearers.")));
				}
			}
			else if (bFaceless)
			{
				Package.Notes.Add(TEXT("Nails upload without a face: they are fitted from the body alone."));
			}
			else if (!bHair)
			{
				Issues.Add(FMantleCosmeticChecks::Info(TEXT("No MetaHuman face was found next to the body, so collars are fitted from the body alone.")));
			}

			if (bHair && !bFace)
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("hair needs the MetaHuman face it was authored on for head fitting"), TEXT("Choose the authoring MetaHuman face next to Authored on; align the cards to that head.")));
				return Package;
			}
			// Bought cards are made for another head. Size and seat them on this one before anything measures them
			// (the preview, the LODs and the upload all read the fitted positions). MetaHuman's own hair is never touched.
			if (bHair && bHairCards && bFace && !Draft.bPlaceHairByHand && ShouldFitThirdPartyHairCardsToHead(Source))
			{
				FitThirdPartyHairCardsToHead(Mesh, FaceMesh, Package);
			}
			FBodyShapeResult Shape;
			if (!bHair) Shape = AddBodyShape(Package, Body, bFace ? &FaceSurface : nullptr, Contract);
			if (!bHair && !Shape.bOk)
			{
				return Package;
			}

			// The worker's bind-pose check (cook.checkGarmentBindPose), here with the plugin's words.
			if (!bHair)
			{
				TArray<FTransform> Component;
				Component.SetNum(Mesh.Glb.Joints.Num());
				TArray<bool> Weighted;
				Weighted.Init(false, Mesh.Glb.Joints.Num());
				for (int32 Influence = 0; Influence < Mesh.Glb.InfluenceJoints.Num(); ++Influence)
				{
					if (Mesh.Glb.InfluenceWeights[Influence] > 0.0f)
					{
						Weighted[Mesh.Glb.InfluenceJoints[Influence]] = true;
					}
				}
				for (int32 Joint = 0; Joint < Mesh.Glb.Joints.Num(); ++Joint)
				{
					const FMantleGlbJoint& Bone = Mesh.Glb.Joints[Joint];
					Component[Joint] = Bone.Parent == INDEX_NONE ? Bone.Local : Bone.Local * Component[Bone.Parent];
					if (!Weighted[Joint])
					{
						continue;
					}
					const FTransform* BodyPose = Shape.ComponentPose.Find(Bone.Name);
					double Distance = 0.0, Degrees = 0.0;
					if (!BodyPose)
					{
						Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("bone \"%s\" carries weight but is not in the body shape's skeleton."), *Bone.Name.ToString())));
					}
					else if (!FMantleCosmeticChecks::WithinBindTolerance(Component[Joint], *BodyPose, Contract, &Distance, &Degrees))
					{
						Issues.Add(FMantleCosmeticChecks::Error(
							FString::Printf(TEXT("bone \"%s\" is bound %.2f cm and %.2f degrees from the body shape's reference pose; upload the garment with the body it was authored on."),
								*Bone.Name.ToString(), Distance, Degrees),
							TEXT("Choose, under Authored on, the MetaHuman body this garment was skinned to.")));
						break;
					}
				}
				// Long garments are accepted and marked as ones that may clip in motion.
				if (Mesh.Bounds.IsValid && Mesh.Bounds.Min.Z < Shape.KneeZ && Draft.Slot != TEXT("footwear"))
				{
					Issues.Add(FMantleCosmeticChecks::Warning(TEXT("this garment hangs below the knee, so it may clip through the legs when the wearer moves."),
						TEXT("It can still be published: Mantle marks it \"may clip in motion\" so players know.")));
				}

				// Coverage over the authoring body (limits.json skinned_garment.coverage).
				TArray<uint32> AllIndices;
				for (const FMantleGlbPrimitive& Primitive : Mesh.Glb.Primitives)
				{
					for (const uint32 Index : Primitive.Indices)
					{
						AllIndices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
					}
				}
				FMantleGarmentCoverage Coverage;
				if (FMantleGarmentCoverage::Compute(FMantleGarmentSurface(Mesh.Glb.Positions, Mesh.Glb.Normals, AllIndices),
					Shape.Positions, Shape.Normals, Coverage, Contract.CoverageThresholdCm, &Error))
				{
					FMantleAssetUploadOptions Options;
					Options.Kind = TEXT("coverage");
					Package.AddAsset(TEXT("coverage"), TEXT("What it covers"), Options, PackCoverageBits(Coverage.Covered, Contract.TopologyVertexCount));
					Package.CoverageKey = TEXT("coverage");
					Package.Notes.Add(FString::Printf(TEXT("Coverage: %d of %d body vertices"), Coverage.NumCovered, Contract.TopologyVertexCount));
				}
				else
				{
					Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("what the garment covers could not be measured (%s); players' clients measure it themselves."), *Error)));
				}
			}
			if (FMantleCosmeticChecks::HasErrors(Issues))
			{
				return Package;
			}

			// A garment's LODs, generated from the uploaded LOD (hair keeps one: see the class comment).
			if (!bHair)
			{
				// Upload identity includes the immutable source used by the cook, even when
				// native resizing leaves garment geometry unchanged across source revisions.
				const FMantleCosmeticUploadAsset* AuthoringSource = Package.FindAsset(Package.SourceShapeKey);
				if (!AuthoringSource) { Issues.Add(FMantleCosmeticChecks::Error(TEXT("the garment has no exported authoring source"))); return Package; }
				Mesh.Glb.AuthoringSourceHash = AuthoringSource->Hash;
			}
			TArray<FMeshSource> LowerLODs;
			if (bHair && Draft.bGenerateLODs)
			{
				int32 Previous = Mesh.NumTriangles;
				for (int32 LOD = 1; LOD < Contract.LODTargetFractions.Num() && LOD < FMantleGlbWriter::MaxLODs; ++LOD)
				{
					FMeshSource Lower = Mesh;
					if (!ReduceHairCards(Lower, FMath::FloorToInt(Mesh.NumTriangles * Contract.LODTargetFractions[LOD]), Error) || Lower.NumTriangles >= Previous)
					{
						Issues.Add(FMantleCosmeticChecks::Warning(TEXT("no smaller whole-card LOD can be made; the remaining hair LODs are omitted"), TEXT("Author independent cards or provide coarser cards geometry. Thin cards are never collapsed.")));
						break;
					}
					Previous = Lower.NumTriangles;
					Package.Notes.Add(FString::Printf(TEXT("Hair card LOD%d: %d triangles"), LOD, Lower.NumTriangles));
					LowerLODs.Add(MoveTemp(Lower));
				}
			}
			else if (!bHair && Draft.bGenerateLODs)
			{
				LowerLODs = MakeLowerLODs(Cast<USkeletalMesh>(Source), Mesh, Contract, TEXT("Garment"), Package, Draft.KeepSlots.GetPtrOrNull());
				for (FMeshSource& Lower : LowerLODs)
				{
					int32 Far = 0;
					double MeanCm = 0.0;
					if (BodySkin.IsSkinned() && !FMantleGarmentOutfit::TransferWeights(Lower.Glb, BodySkin, Far, MeanCm, Error))
					{
						Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("a lower LOD could not take the body's skin (%s)."), *Error)));
						return Package;
					}
				}
			}
			if (bHair && OutHairPreview) *OutHairPreview = PreviewMesh(Mesh, LowerLODs);
			if (bHair && OutHairSourceFace && bFace) *OutHairSourceFace = PreviewMesh(FaceMesh);
			// The immutable cards upload identity includes its exact authoring head. Identical
			// geometry authored against a revised face must not reuse a different cook source.
			if (bHair)
			{
				if (!AddFaceAsset(Package, AuthoringFace, FaceMesh, Draft.bGenerateLODs && !bGeneratedHeadSource, Contract,
					FString::Printf(TEXT("Head it was authored with (%s LOD %d)"), *AuthoringFace->GetName(), FaceMesh.LODIndex), Error))
				{ Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the authoring head could not be written: %s"), *Error))); return Package; }
				Mesh.Glb.AuthoringSourceHash = Package.FindAsset(Package.FaceMeshKey)->Hash;
			}
			TArray<uint8> Glb;
			if (!WriteMeshWithLODs(Mesh, LowerLODs, Glb, Error))
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the mesh could not be written: %s."), *Error)));
				return Package;
			}
			FMantleAssetUploadOptions Options;
			Options.Kind = TEXT("skinnedmesh");
			if (bHair)
			{
				Options.Part = TEXT("hair");
			}
			FMantleCosmeticUploadAsset& Asset = Package.AddAsset(TEXT("mesh"),
				FString::Printf(TEXT("%s mesh (%s, %d triangles)"), bHair ? TEXT("Hair") : TEXT("Garment"), *Source->GetName(), Mesh.NumTriangles),
				Options, MoveTemp(Glb));
			Asset.SourceKey = bHair ? Package.FaceMeshKey : FString(TEXT("shape"));
			Package.MeshKey = TEXT("mesh");

			// The immutable approved source face drives head fitting for hair and collar fitting for clothing.
			if (bFace && !bHair)
			{
				FString FaceError;
				if (!AddFaceAsset(Package, AuthoringFace, FaceMesh, Draft.bGenerateLODs, Contract,
					FString::Printf(TEXT("Face it was authored with (%s LOD %d)"), *AuthoringFace->GetName(), FaceMesh.LODIndex), FaceError))
				{
					Issues.Add(bHair ? FMantleCosmeticChecks::Error(FString::Printf(TEXT("the authoring head could not be written: %s"), *FaceError)) : FMantleCosmeticChecks::Warning(FString::Printf(TEXT("the face it was authored with could not be written (%s), so it uploads without it."), *FaceError),
						TEXT("It can still be published; collars are then fitted from the body alone.")));
				}
			}

			if (bHair && !GroomAtlases.IsEmpty())
			{
				for (int32 M = 0; M < Mesh.Glb.MaterialNames.Num(); ++M)
				{
					FMantleCosmeticMaterialRef Ref; Ref.Role = MantleHairCardsRole; Ref.NativeHairProfile = GroomNativeProfiles[M];
					Ref.NativeHair = GroomNativeSettings[M];
					Ref.NativeHairTangentKey = AddTexture(Package, nullptr, &GroomTangents[M], ETextureUse::Data, Contract.HairMaxTextureSize, FString::Printf(TEXT("Native hair tangent %s"), *Mesh.Glb.MaterialNames[M]), Contract);
					TArray<const UMaterialInterface*> Colors; for (auto* C : GroomColorSources[M]) Colors.Add(C);
					FMantleHairColor::ESource ColorSource;
					Ref.HairColor = FMantleHairColor::Resolve(Draft.HairColor, Colors, ColorSource);
					Ref.AlbedoKey = AddTexture(Package, nullptr, &GroomAtlases[M], ETextureUse::Data, Contract.HairMaxTextureSize, FString::Printf(TEXT("Hair group atlas %s"), *Mesh.Glb.MaterialNames[M]), Contract);
					Package.Materials.Add(Ref);
				}
			}
			else if (bHair && CardTextures.Num() > 0)
			{
				// MetaHuman's cards layout 2: the attribute atlas (coverage R, depth G, seed B) and the
				// tangent atlas (tangent RGB, CoordU A), both data, not colour. The runtime hair material
				// (M_MantleHair's HairAtlas) reads a hair_cards section's coverage and seed from the first.
				UTexture2D* Attribute = nullptr;
				UTexture2D* Tangent = nullptr;
				for (UTexture2D* Texture : CardTextures)
				{
					if (!Tangent && Texture->GetName().Contains(TEXT("Tangent")))
					{
						Tangent = Texture;
					}
					else if (!Attribute && (Texture->GetName().Contains(TEXT("Attribute")) || Texture->GetName().Contains(TEXT("Coverage"))))
					{
						Attribute = Texture;
					}
				}
				Attribute = Attribute ? Attribute : CardTextures[0];
				Tangent = Tangent ? Tangent : (CardTextures.IsValidIndex(1) && CardTextures[1] != Attribute ? CardTextures[1] : nullptr);
				// The atlases hold coverage, not colour, so the sections say hair_cards and carry the colour the
				// cards are drawn in: the creator's, else the groom's material's (MetaHuman keeps it in Melanin and Redness).
				for (UMaterialInterface* Material : Mesh.Materials)
				{
					if (Material)
					{
						HairColorSources.AddUnique(Material);
					}
				}
				FMantleHairColor::ESource ColorSource = FMantleHairColor::ESource::Default;
				const UMaterialInterface* ColorMaterial = nullptr;
				TArray<const UMaterialInterface*> ColorCandidates;
				for (const UMaterialInterface* Material : HairColorSources)
				{
					ColorCandidates.Add(Material);
				}
				const FLinearColor HairColor = FMantleHairColor::Resolve(Draft.HairColor, ColorCandidates, ColorSource, &ColorMaterial);
				Package.Notes.Add(FString::Printf(TEXT("Hair colour %s (linear %.3f, %.3f, %.3f) from %s%s"), *FMantleHairColor::ToHex(HairColor),
					HairColor.R, HairColor.G, HairColor.B, FMantleHairColor::Describe(ColorSource),
					ColorMaterial ? *FString::Printf(TEXT(" (%s)"), *ColorMaterial->GetName()) : TEXT("")));
				if (ColorSource == FMantleHairColor::ESource::Default)
				{
					Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(
						TEXT("no hair colour was found in the groom's materials, so its cards upload in a default brown (%s); set one under Hair colour."),
						*FMantleHairColor::ToHex(HairColor))));
				}
				for (int32 Index = 0; Index < Mesh.Glb.MaterialNames.Num(); ++Index)
				{
					FMantleCosmeticMaterialRef Ref;
					Ref.Role = MantleHairCardsRole;
					Ref.HairColor = HairColor;
					Ref.AlbedoKey = AddTexture(Package, Attribute, nullptr, ETextureUse::Data, Contract.HairMaxTextureSize, TEXT("Hair attribute atlas"), Contract);
					if (Tangent)
					{
						Ref.NormalKey = AddTexture(Package, Tangent, nullptr, ETextureUse::Data, Contract.HairMaxTextureSize, TEXT("Hair tangent atlas"), Contract);
					}
					Package.Materials.Add(Ref);
				}
			}
			else
			{
				// Hair keeps a separate opacity texture in its albedo's alpha (the shipped client's hair mask).
				AddSectionMaterials(Package, Mesh, true, false, Class, TArray<FString>(), bHair ? TEXT("Hair") : TEXT("Garment"), Package.Materials, Contract, bHair);
			}
			if (!bHair)
			{
				Package.MaterialNames = Mesh.Glb.MaterialNames;
				AddClothingColors(Package, Mesh, Draft.ClothingColors, Class, Contract);
				AddDetectedClothingColors(Package, Mesh.Glb, Mesh.Materials, Draft, Class.AlbedoMax, Contract);
			}
			Package.Notes.Add(FString::Printf(TEXT("Mesh: %d vertices, %d triangles, %d sections, %d bones"),
				Mesh.Glb.Positions.Num(), Mesh.NumTriangles, Mesh.Glb.MaterialNames.Num(), Mesh.Glb.Joints.Num()));
		}
		break;

	case EMantleCosmeticClass::Accessory:
		{
			UStaticMesh* Static = Cast<UStaticMesh>(Source);
			if (!Static)
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("an accessory uploads a static mesh whose pivot is where it attaches.")));
				return Package;
			}
			FMeshSource Mesh;
			if (!ReadStaticLOD(Static, 0, Mesh, Error))
			{
				Issues.Add(FMantleCosmeticChecks::Error(Error));
				return Package;
			}
			const FMantleContractClass& Class = Contract.RigidAccessory;
			// An accessory over the triangle limit is reduced automatically, as an over-limit place mesh is:
			// a transient copy with the same material slot stands in and is what uploads. The creator's asset is never
			// touched. A copy has one LOD, so the source's authored LODs are left off with it.
			const UStaticMesh* ReadFrom = Static;
			if (Class.MaxTriangles > 0 && Mesh.NumTriangles > Class.MaxTriangles)
			{
				FMantlePlaceAssetLimits Limits;
				Limits.MaxTriangles = Class.MaxTriangles;
				FString ReduceWhy;
				FMeshSource Reduced;
				const UStaticMesh* Copy = FMantleMeshReduce::ReducedCopy(Static, Limits, ReduceWhy);
				if (Copy && ReadStaticLOD(Copy, 0, Reduced, ReduceWhy))
				{
					Package.Notes.Add(FString::Printf(TEXT("Accessory: reduced from %d to %d triangles to fit the %d limit."), Mesh.NumTriangles, Reduced.NumTriangles, Class.MaxTriangles));
					Mesh = MoveTemp(Reduced);
					ReadFrom = Copy;
				}
				else
				{
					Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("The accessory could not be reduced automatically: %s"), *ReduceWhy),
						TEXT("Lower its triangle count in a modelling tool, or add a smaller LOD0.")));
				}
			}
			// A model exported with its pivot at the feet is out of the slot's reach: its box is moved onto the pivot in the
			// upload (the creator's asset is untouched), and the note says so.
			FVector3f Recenter = FVector3f::ZeroVector;
			if (const FMantleContractAccessoryBounds* SlotLimits = Contract.AccessoryBounds.Find(Draft.Slot))
			{
				Recenter = FMantleAccessorySettings::RecenterOffset(Mesh.Bounds, *SlotLimits);
			}
			if (!Recenter.IsZero())
			{
				for (FVector3f& Position : Mesh.Glb.Positions) Position -= Recenter;
				Mesh.Bounds = Mesh.Bounds.ShiftBy(-Recenter);
				Package.Notes.Add(FString::Printf(TEXT("Accessory: its pivot was out of reach of %s, so the mesh is centred on the socket (moved %.1f, %.1f, %.1f cm)."),
					*Draft.Slot, Recenter.X, Recenter.Y, Recenter.Z));
			}
			FMantleCosmeticChecks::CheckMeshLimits(Mesh.NumTriangles, Mesh.Glb.Positions.Num(), 0, 0, Class, Issues, 1, EMantleLODPick::LODZeroOnly);
			if (Mesh.Glb.MaterialNames.Num() != 1)
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("an accessory has exactly 1 material; %s has %d."), *Static->GetName(), Mesh.Glb.MaterialNames.Num()),
					TEXT("Merge its materials into one.")));
			}
			FMantleCosmeticChecks::CheckAccessoryBounds(Draft.Slot, Mesh.Bounds, Contract, Issues);
			CheckSectionUVTiles(Mesh, TEXT("section"), Issues);
			if (FMantleCosmeticChecks::HasErrors(Issues))
			{
				return Package;
			}
			// Preserve authored static LODs through the existing MRM2 path, including the mesh's pivot and switch sizes.
			TArray<FMantleGlbMesh> LODs;
			LODs.Add(Mesh.Glb);
			FMantleGlbStaticExtras Extras;
			const FStaticMeshRenderData* Render = ReadFrom->GetRenderData();
			const int32 Wanted = Draft.bGenerateLODs ? FMath::Min(Render ? Render->LODResources.Num() : 1,
				FMath::Min(Contract.PlaceMeshMaxLODs, FMantleGlbWriter::MaxLODs)) : 1;
			int32 PreviousTriangles = Mesh.NumTriangles;
			for (int32 Index = 1; Index < Wanted; ++Index)
			{
				FMeshSource Lower;
				TArray<FMantleCosmeticIssue> LowerIssues;
				FString Why;
				if (!ReadStaticLOD(ReadFrom, Index, Lower, Why))
				{
					LowerIssues.Add(FMantleCosmeticChecks::Error(Why));
				}
				else
				{
					for (FVector3f& Position : Lower.Glb.Positions) Position -= Recenter;
					Lower.Bounds = Lower.Bounds.ShiftBy(-Recenter);
					FMantleCosmeticChecks::CheckMeshLimits(Lower.NumTriangles, Lower.Glb.Positions.Num(), 0, 0, Class, LowerIssues);
					FMantleCosmeticChecks::CheckAccessoryBounds(Draft.Slot, Lower.Bounds, Contract, LowerIssues);
					CheckSectionUVTiles(Lower, TEXT("section"), LowerIssues);
					if (Lower.NumTriangles > PreviousTriangles || Lower.Glb.MaterialNames.Num() != 1 || Lower.SlotIndices != Mesh.SlotIndices)
					{
						LowerIssues.Add(FMantleCosmeticChecks::Error(TEXT("each accessory LOD must have no more triangles than the previous LOD and use LOD0's single material slot.")));
					}
				}
				if (FMantleCosmeticChecks::HasErrors(LowerIssues))
				{
					Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("Accessory LOD%d and later LODs were left off: %s"), Index,
						LowerIssues.Num() > 0 ? *LowerIssues[0].Message : TEXT("invalid mesh")), TEXT("Rebuild the authored static LODs and check again.")));
					break;
				}
				PreviousTriangles = Lower.NumTriangles;
				Lower.Glb.MaterialNames = Mesh.Glb.MaterialNames;
				LODs.Add(MoveTemp(Lower.Glb));
			}
			if (LODs.Num() > 1)
			{
				bool bOrdered = true;
				for (int32 Index = 0; Index < LODs.Num(); ++Index)
				{
					const float Size = Render->ScreenSize[Index].Default;
					bOrdered &= FMath::IsFinite(Size) && Size > 0.0f && Size <= FMantleStaticMeshReader::MaxScreenSize && (Index == 0 || Size < Extras.ScreenSizes.Last());
					Extras.ScreenSizes.Add(Size);
				}
				if (!bOrdered)
				{
					Issues.Add(FMantleCosmeticChecks::Warning(TEXT("Accessory LOD screen sizes must be positive and decrease; only LOD0 was kept."), TEXT("Set descending screen sizes in the Static Mesh editor.")));
					LODs.SetNum(1);
					Extras.ScreenSizes.Reset();
				}
			}
			TArray<uint8> Glb;
			if (!FMantleGlbWriter::WriteStatic(LODs, Extras, Glb, Error))
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the mesh could not be written: %s."), *Error)));
				return Package;
			}
			FMantleAssetUploadOptions Options;
			Options.Kind = TEXT("mesh");
			Package.AddAsset(TEXT("mesh"), FString::Printf(TEXT("Accessory mesh (%s, %d triangles)"), *Static->GetName(), Mesh.NumTriangles), Options, MoveTemp(Glb));
			Package.MeshKey = TEXT("mesh");
			Package.Notes.Add(FString::Printf(TEXT("Accessory: %d LODs; rigid, opaque albedo material with an optional normal map; no collision or physics."), LODs.Num()));
			// An accessory takes an optional normal map within the class's normalMax, as a garment does.
			AddSectionMaterials(Package, Mesh, Class.NormalMax > 0, false, Class, TArray<FString>(), TEXT("Accessory"), Package.Materials, Contract);
			// Colour areas players can change, found and measured as clothing's are.
			Package.MaterialNames = Mesh.Glb.MaterialNames;
			AddClothingColors(Package, Mesh, Draft.ClothingColors, Class, Contract);
			AddDetectedClothingColors(Package, Mesh.Glb, Mesh.Materials, Draft, Class.AlbedoMax, Contract);
			Package.bAttach = Draft.bAttach;
			Package.AttachLocation = FMantleCosmeticPackage::ClampAttachLocation(Draft.AttachLocation);
			Package.AttachRotation = FMantleCosmeticPackage::ClampAttachRotation(Draft.AttachRotation);
			Package.AttachScale = FMantleCosmeticPackage::ClampAttachScale(Draft.AttachScale);
		}
		break;

	case EMantleCosmeticClass::Avatar:
		{
			USkeletalMesh* Body = Cast<USkeletalMesh>(Source);
			if (!Body)
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("an avatar uploads a body skeletal mesh (and optionally a MetaHuman face).")));
				return Package;
			}
			const FMantleContractClass& Class = Contract.AvatarBody;
			const int32 ReferenceLOD = FindReferenceLOD(Body, Contract);
			const int32 LOD = AvatarBodyLOD(Body, Contract);
			FMeshSource Mesh;
			if (!ReadSkeletalLOD(Body, LOD, Mesh, Error))
			{
				Issues.Add(FMantleCosmeticChecks::Error(Error));
				return Package;
			}
			int32 Dropped = 0;
			// On a skeleton with the jaw slot, the picked jaw (Draft.JawBone, the body's own name for it) is kept as it.
			// And on one with the secondary motion slots, the mapped chains (Draft.SecondaryMotion) as them.
			FMantleAvatarBodyFit BodyFit;
			const FMantleContractSkeleton* Skeleton = FitAvatarBodySkeleton(Mesh, Draft.JawBone, Draft.SecondaryMotion, Contract, Dropped, BodyFit, Error);
			const EMantleJawSlot JawSlot = BodyFit.Jaw;
			if (!Skeleton)
			{
				Issues.Add(FMantleCosmeticChecks::Error(FMantleCosmeticChecks::EndWithFullStop(Error), JawSlot == EMantleJawSlot::Refused
					? TEXT("Pick the bone that opens the mouth, one below head, in Jaw bone (optional), or pick none.")
					: BodyFit.bSecondaryRefused
					? TEXT("In Secondary motion, map each slot to the first bone of one chain below the slot's parent bone, each bone in one chain only, or pick None.")
					: TEXT("An avatar is skinned to the MetaHuman body skeleton, the UE5 Mannequin skeleton (SK_Mannequin), the UE4 Mannequin skeleton (SK_UE4_Mannequin) or the UEFN Mannequin skeleton (SK_UEFN_Mannequin).")));
				return Package;
			}
			const bool bClientAppearance = Draft.AvatarPublicationMode == TEXT("converted");
			if (bClientAppearance && (Skeleton->Id != TEXT("metahuman") || ReferenceLOD == INDEX_NONE))
			{
				Issues.Add(FMantleCosmeticChecks::Error(TEXT("Converted client-appearance uploads require a canonical MetaHuman body with Mantle's clothing-compatible topology.")));
				return Package;
			}
			// The cook refuses a body missing a bone its retargeting and sockets need, with a scaled bone, or in
			// another pose than its skeleton's own, first; checked on the joint table the GLB will carry.
			FString ShapeError;
			if (!FMantleCosmeticChecks::CheckAvatarBodyShape(Mesh.Glb.Joints, *Skeleton, Contract, ShapeError))
			{
				const TCHAR* ShapeHint = ShapeError.StartsWith(TEXT("the skin has no"), ESearchCase::CaseSensitive)
					? TEXT("Export the whole body skeleton: keep every bone of the MetaHuman, UE5 Mannequin, UE4 Mannequin or UEFN Mannequin body skeleton, even ones no vertex uses.")
					: ShapeError.Contains(TEXT("has scale"), ESearchCase::CaseSensitive)
					? TEXT("Apply the scale to the mesh and skeleton in your modelling tool (every bone at scale 1), then reimport.")
					: TEXT("Pose the body in its skeleton's reference pose before exporting it: arms angled down and out, legs straight down, as the stock MetaHuman, UE5 Mannequin, UE4 Mannequin or UEFN Mannequin body stands. A T-pose or arms-down body is refused.");
				Issues.Add(FMantleCosmeticChecks::Error(FMantleCosmeticChecks::EndWithFullStop(ShapeError), ShapeHint));
			}
			if (Skeleton->Id.Equals(TEXT("ue5mannequin"), ESearchCase::CaseSensitive))
			{
				// The second appearance skeleton. No reference topology and no face.
				Issues.Add(FMantleCosmeticChecks::Info(FString(FMantleCosmeticChecks::UE5MannequinAvatarMessage) + TEXT(".")));
			}
			else if (Skeleton->Id.Equals(TEXT("ue4mannequin"), ESearchCase::CaseSensitive))
			{
				// The third. No reference topology and no face either.
				Issues.Add(FMantleCosmeticChecks::Info(FString(FMantleCosmeticChecks::UE4MannequinAvatarMessage) + TEXT(".")));
			}
			else if (Skeleton->Id.Equals(TEXT("uefnmannequin"), ESearchCase::CaseSensitive))
			{
				// The fourth, the driver's own skeleton. No reference topology and no face either.
				Issues.Add(FMantleCosmeticChecks::Info(FString(FMantleCosmeticChecks::UEFNMannequinAvatarMessage) + TEXT(".")));
			}
			if (ReferenceLOD != INDEX_NONE)
			{
				Issues.Add(FMantleCosmeticChecks::Info(FString::Printf(TEXT("Garment-compatible: LOD %d has Mantle's reference topology, so it takes 3D clothing."), LOD)));
			}
			else if (FMantleAvatarConverter::IsConvertOutput(Body))
			{
				// A converted body without the reference topology is not garment-ready.
				Issues.Add(FMantleCosmeticChecks::Warning(FString(FMantleCosmeticChecks::NotClothingReadyMessage) + TEXT("."),
					FMantleCosmeticChecks::NotClothingReadyHint));
			}
			else if (!UMantleSettings::AllowsAsIsAvatars())
			{
				// With as-is uploads off (the default), every avatar must be a MetaHuman Convert.
				Issues.Add(FMantleCosmeticChecks::Error(FString(FMantleCosmeticChecks::ConvertFirstMessage) + TEXT("."),
					FMantleCosmeticChecks::ConvertFirstHint));
			}
			// The reference body and Epic's built MetaHuman body both end at the neck: without a face the avatar is headless, and never uploads.
			if ((ReferenceLOD != INDEX_NONE || FMantleAvatarConverter::IsConvertOutput(Body)) && !Draft.Face.IsValid())
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString(FMantleCosmeticChecks::HeadlessAvatarMessage) + TEXT("."), FMantleCosmeticChecks::HeadlessAvatarHint));
			}
			else if (Skeleton->Id.Equals(TEXT("metahuman"), ESearchCase::CaseSensitive))
			{
				// As-is (what you see is what you get): a supported skeleton and the limits; it wears no 3D clothing.
				Issues.Add(FMantleCosmeticChecks::Warning(FString(FMantleCosmeticChecks::NotGarmentCompatibleMessage) + TEXT("."),
					FMantleCosmeticChecks::NotGarmentCompatibleHint));
			}
			NoteFoldedBones(Mesh, TEXT("the body"), Issues);
			NoteCappedInfluences(Mesh, TEXT("the body"), Issues);
			FMantleCosmeticChecks::CheckMeshLimits(Mesh.NumTriangles, Mesh.Glb.Positions.Num(), Mesh.Glb.MaterialNames.Num(), Mesh.MostInfluences, Class, Issues,
				UMantleBodyTopologyData::GetNumLODs(Body), EMantleLODPick::FirstWithinLimit);
			FMantleCosmeticChecks::CheckSectionNames(Mesh.Glb.MaterialNames, Issues);
			CheckSectionUVTiles(Mesh, TEXT("the body's section"), Issues);
			FMantleCosmeticChecks::CheckAvatarBodyBounds(Mesh.Bounds, Skeleton->Id, Contract, Issues);

			FMeshSource FaceMesh;
			USkeletalMesh* Face = Draft.Face.Get();
			TOptional<float> FaceTop;
			if (bClientAppearance)
			{
				if (!Face || !FMantleAvatarConverter::IsConvertOutput(Body) || !FMantleAvatarConverter::IsConvertOutput(Face))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Face && !FMantleAvatarConverter::IsConvertOutput(Body)
						? TEXT("This body is an assembled MetaHuman (meshes, no MetaHuman Creator character) that no build step ran on: Convert builds it from its face and body, and it could not be built. Converted client-appearance uploads need the converter's own build.")
						: TEXT("Converted client-appearance uploads need the supported converted MetaHuman body and its converted face.")));
					return Package;
				}
				const FString* BodyBuilt = Body->GetPackage()->GetMetaData().FindValue(Body, FMantleAvatarConverter::BuiltKey);
				const FString* FaceBuilt = Face->GetPackage()->GetMetaData().FindValue(Face, FMantleAvatarConverter::BuiltKey);
				if (!BodyBuilt || BodyBuilt->IsEmpty() || !FaceBuilt || *BodyBuilt != *FaceBuilt)
				{
					Issues.Add(FMantleCosmeticChecks::Error(TEXT("The converted MetaHuman body and face must share converter build provenance.")));
					return Package;
				}
			}
			if (Face)
			{
				if (Skeleton->Id != TEXT("metahuman"))
				{
					Issues.Add(FMantleCosmeticChecks::Error(TEXT("only a MetaHuman body takes a MetaHuman face.")));
					return Package;
				}
				// Read as a garment's authoring face is (ReadFaceSource), so the same face uploads as the same bytes.
				if (!ReadFaceSource(Face, Contract, FaceMesh, Error))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Error));
					return Package;
				}
				NoteFoldedBones(FaceMesh, TEXT("the face"), Issues);
				NoteCappedInfluences(FaceMesh, TEXT("the face"), Issues);
				FMantleCosmeticChecks::CheckMeshLimits(FaceMesh.NumTriangles, FaceMesh.Glb.Positions.Num(), FaceMesh.Glb.MaterialNames.Num(),
					FaceMesh.MostInfluences, Contract.AvatarFace, Issues, UMantleBodyTopologyData::GetNumLODs(Face), EMantleLODPick::FirstWithinLimit);
				FMantleCosmeticChecks::CheckSectionNames(FaceMesh.Glb.MaterialNames, Issues);
				CheckSectionUVTiles(FaceMesh, TEXT("the face's section"), Issues);
				FaceTop = FaceMesh.Bounds.IsValid ? FaceMesh.Bounds.Max.Z : 0.0f;
				Package.Notes.Add(FString::Printf(TEXT("Face: %s LOD %d, %d triangles"), *Face->GetName(), FaceMesh.LODIndex, FaceMesh.NumTriangles));
			}
			if (Skeleton->Id == TEXT("metahuman"))
			{
				FMantleCosmeticChecks::CheckAvatarHeight(Mesh.Bounds.IsValid ? Mesh.Bounds.Max.Z : 0.0f, FaceTop, Contract, Issues);
			}
			// How tall the game will play it, measured as the cook will (the face's top on a MetaHuman).
			if (Mesh.Bounds.IsValid)
			{
				Package.AvatarStandingHeightCm = FMantleCosmeticChecks::AvatarStandingHeightCm(Mesh.Bounds.Max.Z, FaceTop);
				Package.Notes.Add(FString::Printf(TEXT("Standing height %.1f cm. %s"), Package.AvatarStandingHeightCm.GetValue(),
					*FMantleCosmeticChecks::DescribeAvatarPlayingHeight(Package.AvatarStandingHeightCm.GetValue(), Contract)));
			}

			// The jaw lip-sync drives: the chosen one, else the face's default. On a skeleton with
			// the jaw slot, the slot when a jaw was kept, else none, with a note either way.
			FName Jaw = FMantleCosmeticChecks::AvatarJawBone(JawSlot, Draft.JawBone);
			TMap<FName, FName> RenamedBones;
			if (JawSlot == EMantleJawSlot::Kept || JawSlot == EMantleJawSlot::NotPicked)
			{
				const bool bKept = JawSlot == EMantleJawSlot::Kept;
				Issues.Add(FMantleCosmeticChecks::JawSlotIssue(bKept ? Draft.JawBone : NAME_None));
				if (bKept)
				{
					RenamedBones.Add(Draft.JawBone, FMantleCosmeticChecks::JawSlotBone);
					UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_JAW %s -> %s"), *Draft.JawBone.ToString(), *FMantleCosmeticChecks::JawSlotBone.ToString());
				}
			}
			// The chains kept as secondary motion slots, their presets on the item, and a note when there were any, or
			// when extra bones folded that could have been.
			if (BodyFit.bSecondarySlots && (BodyFit.Kept.Num() > 0 || Mesh.FoldedBones.Num() > 0))
			{
				Issues.Add(FMantleCosmeticChecks::SecondaryMotionIssue(BodyFit.Kept));
			}
			for (const FMantleSecondaryKept& Kept : BodyFit.Kept)
			{
				const FMantleSecondaryChainDef* Chain = MantleSecondaryMotion::FindChain(Kept.ChainId);
				for (int32 Index = 0; Chain && Index < Kept.SourceBones.Num() && Index < Chain->Bones.Num(); ++Index)
				{
					RenamedBones.Add(Kept.SourceBones[Index], Chain->Bones[Index]);
				}
				Package.SecondaryMotion.Add(TPair<FString, FString>(Kept.ChainId, MantleSecondaryMotion::PresetId(Kept.Preset)));
				UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_SECONDARY %s <- %s (%d bones, %d folded past the end) preset=%s"), *Kept.ChainId,
					Kept.SourceBones.Num() > 0 ? *Kept.SourceBones[0].ToString() : TEXT(""), Kept.SourceBones.Num(), Kept.FoldedPastEnd,
					MantleSecondaryMotion::PresetId(Kept.Preset));
			}
			auto HasBone = [](const FMeshSource& In, FName Bone)
			{
				return In.Glb.Joints.ContainsByPredicate([Bone](const FMantleGlbJoint& Joint) { return Joint.Name == Bone; });
			};
			if (Jaw.IsNone() && Face && HasBone(FaceMesh, Contract.FaceJawBoneDefault))
			{
				Jaw = Contract.FaceJawBoneDefault;
			}
			if (!Jaw.IsNone() && !HasBone(Mesh, Jaw) && !(Face && HasBone(FaceMesh, Jaw)))
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the jaw bone \"%s\" is not in the body's or the face's skeleton."), *Jaw.ToString())));
			}
			Package.JawBone = Jaw;

			// Socket overrides (the item's "sockets"): refused past the API's bounds, never clamped.
			TArray<FMantleCosmeticSocketOverride> SocketOverrides = Draft.SocketOverrides;
			// Hats fit this head: unless the creator placed the hat socket themselves, it is fitted to the measured cranium.
			FMantleCosmeticSocketOverride* Hat = SocketOverrides.FindByPredicate([](const FMantleCosmeticSocketOverride& Entry) { return Entry.Socket == TEXT("acc_hat"); });
			if (Skeleton->Id != TEXT("metahuman"))
			{
				// The fit is proven in the MetaHuman head's axes only; another skeleton's head bone can be turned.
				Package.Notes.Add(FString::Printf(TEXT("Hats use the standard head size on the %s skeleton."), *Skeleton->Id));
			}
			else if (!Hat || (!Hat->bLocation && !Hat->bScale))
			{
				FBox3f Reference;
				FBox3f Cranium;
				int32 CraniumVertices = 0;
				FString HeadError;
				const FTransform SocketInHead = FMantleHeadFit::HatSocketInHead(Body);
				if (!FMantleHeadFit::MeasureCranium(Body, Face, SocketInHead, Cranium, CraniumVertices, HeadError))
				{
					Issues.Add(FMantleCosmeticChecks::Warning(FString::Printf(TEXT("Hats use the standard head size: %s."), *HeadError),
						TEXT("Place the acc_hat socket in Sockets if hats sit wrong on this avatar.")));
				}
				else if (!FMantleHeadFit::ReferenceCranium(Reference))
				{
					Package.Notes.Add(FString::Printf(TEXT("Hat socket left as the skeleton's: the plugin's head reference is missing (this head's cranium min %s max %s)."),
						*Cranium.Min.ToString(), *Cranium.Max.ToString()));
				}
				else
				{
					const FMantleCosmeticSocketOverride Fitted = FMantleHeadFit::HatSocket(Reference, Cranium, SocketInHead);
					if (Hat)
					{
						Hat->bLocation = true;
						Hat->bScale = true;
						Hat->Location = Fitted.Location;
						Hat->Scale = Fitted.Scale;
					}
					else
					{
						SocketOverrides.Add(Fitted);
					}
					Package.Notes.Add(FString::Printf(TEXT("Hats fitted to this head: %s against the reference %s (scale %.2f, %.2f, %.2f)."),
						*FMantleHeadFit::Describe(Cranium), *FMantleHeadFit::Describe(Reference), Fitted.Scale.X, Fitted.Scale.Y, Fitted.Scale.Z));
					UE_LOG(LogMantle, Log, TEXT("MANTLE_HEAD_FIT body=%s face=%s vertices=%d min=%s max=%s location=%s scale=%s"),
						*Body->GetName(), Face ? *Face->GetName() : TEXT("none"), CraniumVertices, *Cranium.Min.ToString(), *Cranium.Max.ToString(),
						*Fitted.Location.ToString(), *Fitted.Scale.ToString());
				}
			}
			FMantleCosmeticPackage::CheckSocketOverrides(SocketOverrides, Contract, Issues);
			for (const FMantleCosmeticSocketOverride& Override : SocketOverrides)
			{
				if (!Override.IsEmpty())
				{
					Package.Sockets.Add(Override);
					Package.Notes.Add(FString::Printf(TEXT("Socket %s overridden:%s%s%s"), *Override.Socket,
						Override.bLocation ? TEXT(" location") : TEXT(""), Override.bRotation ? TEXT(" rotation") : TEXT(""), Override.bScale ? TEXT(" scale") : TEXT("")));
				}
			}
			if (FMantleCosmeticChecks::HasErrors(Issues))
			{
				return Package;
			}

			TArray<FMeshSource> BodyLODs;
			if (ReferenceLOD != INDEX_NONE && Skeleton->Id == TEXT("metahuman"))
			{
				// These complete native graphs define clothing compatibility. A generic
				// simplifier replaces their connectivity, so it must never regenerate them.
				// Preserving authored LODs is independent of the optional generation switch.
				if (!ReadCanonicalNativeBodyLODs(Body, Mesh, BodyLODs, Error, RenamedBones.Num() > 0 ? &RenamedBones : nullptr))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Error, TEXT("Use the complete unclothed MetaHuman body with all three verified native LODs.")));
					return Package;
				}
				Package.Notes.Add(TEXT("Body: preserved all three complete native canonical LODs; no generated body reduction."));
				if (!Mesh.MergedBodySplits.IsEmpty()) Package.Notes.Add(MantleCosmeticExportPrivate::MergedBodySplitsNote(Mesh.MergedBodySplits));
			}
			else if (Draft.bGenerateLODs)
			{
				BodyLODs = MakeLowerLODs(Body, Mesh, Contract, TEXT("Body"), Package, nullptr, RenamedBones.Num() > 0 ? &RenamedBones : nullptr);
			}
			TArray<FString> Roles = DefaultSectionRoles(Body, LOD);
			if (!bClientAppearance)
			{
				for (int32 Index = 0; Index < Roles.Num() && Index < Draft.SectionRoles.Num(); ++Index)
				{
					if (!Draft.SectionRoles[Index].IsEmpty())
					{
						Roles[Index] = Draft.SectionRoles[Index];
					}
				}
			}
			if (bClientAppearance)
			{
				// Resolved before the write: the GLB's sections must already carry the client ids.
				if (!BuildClientAppearanceMaterials(Mesh.Glb, Roles, false, Package.Materials, Error))
				{
					Issues.Add(FMantleCosmeticChecks::Error(Error));
					return Package;
				}
				NameSectionsByClientMaterial(Mesh, BodyLODs, Package.Materials);
				Package.Notes.Add(TEXT("Converted body: exported geometry and rig only; the client supplies the skin material and appearance textures."));
			}

			if (bClientAppearance && Draft.bBodyOffsets)
			{
				// The sliders' body shapes ride the body's GLB and the face's (its neck follows the body).
				AttachBodyOffsets(Body, Mesh, BodyLODs, Face ? &FaceMesh : nullptr, Package, Issues);
			}

			TArray<uint8> Glb;
			if (!WriteMeshWithLODs(Mesh, BodyLODs, Glb, Error, bClientAppearance))
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the body could not be written: %s."), *Error)));
				return Package;
			}
			FMantleAssetUploadOptions Options;
			Options.Kind = TEXT("avatar");
			Package.AddAsset(TEXT("mesh"), FString::Printf(TEXT("Avatar body (%s LOD %d)"), *Body->GetName(), LOD), Options, MoveTemp(Glb));
			Package.MeshKey = TEXT("mesh");

			if (!bClientAppearance)
			{
				AddSectionMaterials(Package, Mesh, true, true, Class, Roles, TEXT("Body"), Package.Materials, Contract);
			}

			if (Face)
			{
				// Generated face reductions have proven semantic eye-slot drift in LOD1/2. Until a
				// corrected reducer proves side bindings, converted packages ship the verified source
				// reference face as one LOD; As-Is keeps its previous selected generation policy.
				const bool bGenerateFaceLODs = Draft.bGenerateLODs && !bClientAppearance;
				TArray<FString> FaceRoles = DefaultSectionRoles(Face, FaceMesh.LODIndex);
				if (bClientAppearance)
				{
					// Resolved before the write, as for the body; converted faces ship one LOD.
					if (!BuildClientAppearanceMaterials(FaceMesh.Glb, FaceRoles, true, Package.FaceMaterials, Error))
					{
						Issues.Add(FMantleCosmeticChecks::Error(Error));
						return Package;
					}
					TArray<FMeshSource> NoFaceLODs;
					NameSectionsByClientMaterial(FaceMesh, NoFaceLODs, Package.FaceMaterials);
					Package.Notes.Add(TEXT("Converted face: exported geometry, reference pose and independent eye-side weights only; client-owned materials and textures provide appearance."));
					if (Draft.bGenerateLODs)
					{
						Package.Notes.Add(TEXT("Converted face LOD generation is held until generated eye section/material bindings pass per-LOD side-weight checks."));
					}
				}
				if (!AddFaceAsset(Package, Face, FaceMesh, bGenerateFaceLODs, Contract,
					FString::Printf(TEXT("Avatar face (%s LOD %d)"), *Face->GetName(), FaceMesh.LODIndex), Error, bClientAppearance))
				{
					Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the face could not be written: %s."), *Error)));
					return Package;
				}
				if (!bClientAppearance)
				{
					AddSectionMaterials(Package, FaceMesh, true, true, Contract.AvatarFace, FaceRoles, TEXT("Face"),
						Package.FaceMaterials, Contract, false, true, FMantleNativeEyeExport::AllowsSharedProfile(Body, Face));
				}
			}
			// The body's DNA is optional and not exported; avatars without it are posed by the retarget alone.
			Package.Notes.Add(FString::Printf(TEXT("Body: %s LOD %d, %d vertices, %d triangles, skeleton %s"),
				*Body->GetName(), LOD, Mesh.Glb.Positions.Num(), Mesh.NumTriangles, *Skeleton->Id));
		}
		break;

	case EMantleCosmeticClass::Overlay:
		// Built above (BuildOverlayPackage) and returned: an overlay has no mesh to export.
		break;

	case EMantleCosmeticClass::Emote:
		{
			UAnimSequence* Sequence = Cast<UAnimSequence>(Source);
			FMantleAnimClipData Clip;
			// A clip on the MetaHuman, UE5 or UE4 Mannequin skeleton is retargeted onto the UEFN mannequin first; one on
			// any other skeleton is sampled as it is, and Validate names its first foreign bone.
			FString SkeletonWhy;
			const FString SourceSkeleton = Sequence ? FMantleEmoteRetarget::IdentifySkeleton(Sequence->GetSkeleton(), Contract, &SkeletonWhy) : FString();
			const FMantleContractSkeleton* EmoteFixture = Contract.FindSkeleton(Contract.EmoteSkeleton);
			if (Sequence && FMantleEmoteRetarget::RetargeterPathFor(SourceSkeleton))
			{
				FString Note;
				if (!FMantleEmoteRetarget::Sample(Sequence, SourceSkeleton, Contract, Clip, Error, Note, Draft.EmoteBody.Get()))
				{
					Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the emote could not be retargeted onto the UEFN mannequin: %s"), *Error)));
					return Package;
				}
				Package.Notes.Add(Note);
			}
			else if (!Sequence || !FMantleAnimClipWriter::Sample(Sequence, Contract, Clip, Error))
			{
				Issues.Add(FMantleCosmeticChecks::Error(Sequence ? Error : FString(TEXT("an emote uploads an animation sequence."))));
				return Package;
			}
			if (!SkeletonWhy.IsEmpty() && !SourceSkeleton.IsEmpty())
			{
				Package.Notes.Add(FString::Printf(TEXT("Emote skeleton %s: %s"), *Sequence->GetSkeleton()->GetName(), *SkeletonWhy));
			}
			if (SourceSkeleton == Contract.EmoteSkeleton && EmoteFixture)
			{
				// A UEFN mannequin with bones of its own: those bones' tracks stay behind (the driver has no such bones).
				const int32 Before = Clip.Tracks.Num();
				Clip.Tracks.RemoveAll([EmoteFixture](const FMantleAnimClipTrackData& Track) { return EmoteFixture->Find(Track.Bone) == INDEX_NONE; });
				if (Clip.Tracks.Num() < Before)
				{
					Package.Notes.Add(FString::Printf(TEXT("Emote: %d track(s) of bones the UEFN mannequin does not have were left out"), Before - Clip.Tracks.Num()));
				}
			}
			else if (SourceSkeleton.IsEmpty() && EmoteFixture && Clip.Tracks.ContainsByPredicate([EmoteFixture](const FMantleAnimClipTrackData& Track) { return EmoteFixture->Find(Track.Bone) == INDEX_NONE; }))
			{
				// A skeleton of its own: say which, and why it is none Mantle retargets from, once, instead of a foreign bone.
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("%s is on the skeleton %s, which Mantle cannot retarget emotes from: %s."),
					*Sequence->GetName(), Sequence->GetSkeleton() ? *Sequence->GetSkeleton()->GetName() : TEXT("(none)"), *SkeletonWhy),
					TEXT("Emotes are made on the UEFN mannequin, a MetaHuman, the UE5 Mannequin or the UE4 Mannequin. Retarget the animation onto SK_UEFN_Mannequin in Unreal (right-click it > Retarget Animations) and upload the retargeted copy.")));
				return Package;
			}
			// The creator's Loop choice: loop until the player moves (from a loop_start notify, else the whole clip), or play once.
			if (FMantleAnimClipWriter::ApplyLoopChoice(Clip, Draft.bEmoteLoop))
			{
				Package.Notes.Add(Draft.bEmoteLoop ? FString(TEXT("Emote: loops whole until the player moves (no loop_start notify)"))
					: FString(TEXT("Emote: plays once (Loop is off, so its loop_start notify was left out)")));
			}
			AddEmoteSamplingNotes(Clip, Contract, Package, Issues);
			FMantleAnimClipWriter::Validate(Clip, Contract, Issues);
			if (FMantleCosmeticChecks::HasErrors(Issues))
			{
				return Package;
			}
			TArray<uint8> Bytes = FMantleAnimClipWriter::Encode(Clip, Contract.FindSkeleton(Contract.EmoteSkeleton));
			if (Bytes.Num() > Contract.EmoteMaxBytes)
			{
				Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the clip is %d bytes, the limit is %d."), Bytes.Num(), Contract.EmoteMaxBytes),
					TEXT("Shorten it, or animate fewer bones.")));
				return Package;
			}
			FMantleAssetUploadOptions Options;
			Options.Kind = TEXT("animation");
			Package.AddAsset(TEXT("clip"), FString::Printf(TEXT("Emote (%s, %.1f s)"), *Sequence->GetName(), Clip.DurationSeconds()), Options, MoveTemp(Bytes));
			Package.ClipKey = TEXT("clip");
			Package.Notes.Add(FString::Printf(TEXT("Emote: %d frames at %g fps, %d tracks, %d events"), Clip.FrameCount, Clip.FrameRate, Clip.Tracks.Num(), Clip.Events.Num()));
		}
		break;
	}

	FString ThumbnailNote;
	FString ThumbnailProblem;
	const FMantleCosmeticUploadAsset* EmoteClipAsset = Package.ClipKey.IsEmpty() ? nullptr : Package.FindAsset(Package.ClipKey);
	TArray<uint8> Thumbnail = MakeThumbnail(Draft, ThumbnailOf, ThumbnailBody, Contract.ThumbnailSize, ThumbnailNote, ThumbnailProblem, ThumbnailFrame,
		EmoteClipAsset ? TConstArrayView<uint8>(EmoteClipAsset->Bytes) : TConstArrayView<uint8>());
	if (!ThumbnailProblem.IsEmpty())
	{
		Issues.Add(FMantleCosmeticChecks::Error(FString::Printf(TEXT("the thumbnail's custom image could not be used: %s"), *ThumbnailProblem),
			TEXT("Choose another picture in the Thumbnail section, or go back to Automatic.")));
		return Package;
	}
	FMantleAssetUploadOptions ThumbnailOptions;
	ThumbnailOptions.Kind = TEXT("texture");
	Package.AddAsset(TEXT("thumbnail"), TEXT("Thumbnail"), ThumbnailOptions, MoveTemp(Thumbnail));
	Package.ThumbnailKey = TEXT("thumbnail");
	Package.Notes.Add(ThumbnailNote);

	for (const FMantleCosmeticUploadAsset& Asset : Package.Assets)
	{
		FMantleCosmeticChecks::CheckUploadSize(Asset.Label, Asset.Bytes.Num(), Contract, Issues);
	}
	return Package;
}
