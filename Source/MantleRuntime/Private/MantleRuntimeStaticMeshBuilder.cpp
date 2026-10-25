// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntimeStaticMeshBuilder.h"

#include "Engine/StaticMesh.h"
#include "MantleRuntime.h"
#include "MantleRuntimeMesh.h"
#include "MantleStaticMeshReader.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderUtils.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleRuntimeStaticMeshBuilderPrivate
{
	UStaticMesh* FailStaticMeshBuild(FString* OutError, const FString& Error)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_STATICMESH FAIL %s"), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	}

	/**
	 * Cuts Buffer to its first Channels texture coordinate channels. UStaticMesh::BuildFromMeshDescription fills
	 * the buffer through MakeConstMeshBuildVertexView, which always hands it MAX_STATIC_TEXCOORDS (8) channels
	 * whatever the mesh description has (UE 5.8), so every fast-built LOD would carry eight UVs a vertex, on the
	 * CPU and the GPU. Tangents are kept byte for byte and each kept UV as stored (a half-precision UV reads
	 * back and writes again unchanged). Before InitResources only: the buffer's CPU data is replaced.
	 */
	void TrimTexCoordChannels(FStaticMeshVertexBuffer& Buffer, const int32 Channels)
	{
		const uint32 NumVertices = Buffer.GetNumVertices();
		const uint32 Keep = static_cast<uint32>(FMath::Max(1, Channels));
		if (NumVertices == 0 || Buffer.GetNumTexCoords() <= Keep || !Buffer.GetTangentData() || !Buffer.GetTexCoordData())
		{
			return;
		}
		TArray<uint8> Tangents;
		Tangents.Append(static_cast<const uint8*>(Buffer.GetTangentData()), Buffer.GetTangentSize());
		TArray<FVector2f> UVs;
		UVs.SetNumUninitialized(static_cast<int32>(NumVertices * Keep));
		for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			for (uint32 Channel = 0; Channel < Keep; ++Channel)
			{
				UVs[static_cast<int32>(Vertex * Keep + Channel)] = Buffer.GetVertexUV(Vertex, Channel);
			}
		}

		const bool bFullPrecisionUVs = Buffer.GetUseFullPrecisionUVs();
		const bool bHighPrecisionTangents = Buffer.GetUseHighPrecisionTangentBasis();
		Buffer.CleanUp();
		Buffer.SetUseFullPrecisionUVs(bFullPrecisionUVs);
		Buffer.SetUseHighPrecisionTangentBasis(bHighPrecisionTangents);
		// The fast build keeps a CPU copy (FStaticMeshVertexBufferFlags::bNeedsCPUAccess); so does this.
		Buffer.Init(NumVertices, Keep, /*bNeedsCPUAccess*/ true);
		FMemory::Memcpy(Buffer.GetTangentData(), Tangents.GetData(), Tangents.Num());
		for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			for (uint32 Channel = 0; Channel < Keep; ++Channel)
			{
				Buffer.SetVertexUV(Vertex, Channel, UVs[static_cast<int32>(Vertex * Keep + Channel)]);
			}
		}
	}

	/** One slot per entry of SlotNames, in slot order. */
	struct FSlotSpec
	{
		TArray<FName> SlotNames;

		/**
		 * Per LOD, the material slot of each render section, keyed by the section's first index in the
		 * built index buffer: one per polygon group that kept a triangle (the fast build writes each
		 * group's triangles contiguously, in creation order, and makes no section for an empty one).
		 */
		TArray<TMap<uint32, int32>> SlotByFirstIndex;
	};

	/**
	 * The shared tail of both builds: the UStaticMesh, its slots, the fast build of every LOD, the
	 * authored screen sizes and the collision. Each description must already hold its LOD's triangles.
	 * Simple is the simple collision, or null.
	 */
	UStaticMesh* FinishStaticMeshBuild(const TArray<FMeshDescription*>& Descriptions, const FSlotSpec& Slots, const TConstArrayView<float> ScreenSizes,
		const FMantleStaticMeshCollision* Simple, const FMantleStaticMeshBuildParams& Params, FString* OutError)
	{
		UObject* MeshOuter = Params.Outer ? Params.Outer : GetTransientPackage();
		const FName MeshName = Params.Name.IsNone() ? NAME_None : MakeUniqueObjectName(MeshOuter, UStaticMesh::StaticClass(), Params.Name);
		UStaticMesh* Built = NewObject<UStaticMesh>(MeshOuter, MeshName, RF_Transient);
		for (const FName& SlotName : Slots.SlotNames)
		{
			Built->GetStaticMaterials().Add(FStaticMaterial(Params.Material, SlotName));
		}

		// A non-editor build reads collision triangles back from the render data, and only from a mesh
		// flagged for CPU access (UStaticMesh::GetPhysicsTriMeshData). Simple collision that also answers
		// complex queries needs no triangles.
		const bool bSimpleCollision = Params.bComplexCollision && Params.bUseSimpleCollision && Simple && Simple->HasShapes();
		const bool bCollisionTriangles = Params.bComplexCollision && !(bSimpleCollision && Simple->bSimpleAsComplex);
		const bool bCPUAccess = Params.bAllowCPUAccess || bCollisionTriangles;
		Built->bAllowCPUAccess = bCPUAccess;

		// UStaticMesh::BuildFromMeshDescriptions' fast path (UE 5.8 StaticMesh.cpp, for a mesh with no render
		// resources yet), step for step, with one step added: each LOD's texture coordinates are cut to the
		// channels its description has before the resources are initialised (TrimTexCoordChannels). No source
		// model is kept (a non-editor build has none to keep).
		if (Descriptions.Num() < 1 || Descriptions.Num() > MAX_STATIC_MESH_LODS)
		{
			return FailStaticMeshBuild(OutError, FString::Printf(TEXT("%d LODs (1 to %d)"), Descriptions.Num(), MAX_STATIC_MESH_LODS));
		}
		Built->NeverStream = true;
		Built->bDoFastBuild = true;
		Built->SetRenderData(MakeUnique<FStaticMeshRenderData>());
		FStaticMeshRenderData* RenderData = Built->GetRenderData();
		RenderData->AllocateLODResources(Descriptions.Num());
		for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
		{
			FStaticMeshLODResources& LODResources = RenderData->LODResources[LODIndex];
			LODResources.IndexBuffer.TrySetAllowCPUAccess(bCPUAccess);
			FStaticMeshAttributes Attributes(*Descriptions[LODIndex]);
			auto UVs = Attributes.GetVertexInstanceUVs();
			const int32 AuthoredChannels = FMath::Max(1, UVs.GetNumChannels());
			// The fast engine builder fills MAX_STATIC_TEXCOORDS, even for a one-channel description.
			// Initialize every channel it may read, then retain only the format's actual authored channels.
			// This mutates only our temporary description, never a source asset or the decoded mesh.
			UVs.SetNumChannels(MAX_STATIC_TEXCOORDS);
			for (const FVertexInstanceID Instance : Descriptions[LODIndex]->VertexInstances().GetElementIDs())
			{
				for (int32 Channel = AuthoredChannels; Channel < MAX_STATIC_TEXCOORDS; ++Channel)
				{
					UVs.Set(Instance, Channel, UVs.Get(Instance, 0));
				}
			}
			Built->BuildFromMeshDescription(*Descriptions[LODIndex], LODResources);
			TrimTexCoordChannels(LODResources.VertexBuffers.StaticMeshVertexBuffer, AuthoredChannels);
		}
		// A mesh built for collision alone (a dedicated server's) keeps its CPU buffers and gets no render
		// resources: UStaticMesh::InitResources asserts in a cooked run that can never render.
		if (FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources(Params.bInitRenderResources))
		{
			bool bRayTracing = Built->bSupportRayTracing;
#if !WITH_EDITOR
			bRayTracing = bRayTracing && IsRayTracingAllowed();
#endif
			if (bRayTracing)
			{
				RenderData->InitializeRayTracingRepresentationFromRenderingLODs();
			}
			Built->InitResources();
		}
		RenderData->Bounds = Descriptions[0]->GetBounds();
		Built->CalculateExtendedBounds();
		for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
		{
			// The engine's own default, replaced below by an MRM2's authored sizes.
			RenderData->ScreenSize[LODIndex].Default = FMath::Pow(0.75f, static_cast<float>(LODIndex));
		}
		Built->CreateBodySetup();
		if (UBodySetup* CreatedBody = Built->GetBodySetup())
		{
			CreatedBody->InvalidatePhysicsData();
			// SetStaticMesh can ask the body setup for physics meshes even on a NoCollision component.
			// A render-only mesh has no retained collision indices and must never attempt that cook.
			CreatedBody->bNeverNeedsCookedCollisionData = !Params.bComplexCollision;
		}

		// Bind every render section to its material slot explicitly. The fast build derives a section's
		// MaterialIndex from its polygon group's slot name, which does not always match the slot the MRM2
		// section names; the slot, not the name lookup, is the contract (materials[i] is slot i).
		if (!RenderData || RenderData->LODResources.Num() != Descriptions.Num() || Slots.SlotByFirstIndex.Num() != Descriptions.Num())
		{
			return FailStaticMeshBuild(OutError, FString::Printf(TEXT("the engine built %d LODs of %d"),
				RenderData ? RenderData->LODResources.Num() : 0, Descriptions.Num()));
		}
		for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
		{
			FStaticMeshSectionArray& Sections = RenderData->LODResources[LODIndex].Sections;
			const TMap<uint32, int32>& SlotByFirstIndex = Slots.SlotByFirstIndex[LODIndex];
			if (Sections.Num() != SlotByFirstIndex.Num())
			{
				return FailStaticMeshBuild(OutError, FString::Printf(TEXT("the engine built %d sections for LOD%d's %d material sections"),
					Sections.Num(), LODIndex, SlotByFirstIndex.Num()));
			}
			for (int32 Section = 0; Section < Sections.Num(); ++Section)
			{
				const int32* Slot = SlotByFirstIndex.Find(Sections[Section].FirstIndex);
				if (!Slot)
				{
					return FailStaticMeshBuild(OutError, FString::Printf(TEXT("the engine built a section of LOD%d at index %u, which no material section starts at"),
						LODIndex, Sections[Section].FirstIndex));
				}
				if (Sections[Section].MaterialIndex != *Slot)
				{
					UE_LOG(LogMantle, Verbose, TEXT("MANTLE_STATICMESH LOD%d section %d bound to slot %d (the engine chose %d)"),
						LODIndex, Section, *Slot, Sections[Section].MaterialIndex);
					Sections[Section].MaterialIndex = *Slot;
				}
				Sections[Section].bEnableCollision = Params.bComplexCollision;
#if WITH_EDITOR
				FMeshSectionInfo Info = Built->GetSectionInfoMap().Get(LODIndex, Section);
				Info.MaterialIndex = *Slot;
				Info.bEnableCollision = Params.bComplexCollision;
				Built->GetSectionInfoMap().Set(LODIndex, Section, Info);
#endif
			}
		}

		// The authored switch points. The fast build sets 0.75^LOD, which is nobody's choice.
		if (Descriptions.Num() > 1 && ScreenSizes.Num() == Descriptions.Num())
		{
			for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
			{
				RenderData->ScreenSize[LODIndex].Default = ScreenSizes[LODIndex];
			}
		}

		if (Params.bComplexCollision)
		{
			UBodySetup* Body = Built->GetBodySetup();
			if (!Body)
			{
				return FailStaticMeshBuild(OutError, TEXT("the engine made no body setup for the mesh"));
			}
			if (bSimpleCollision)
			{
				// The creator's own shapes, as they are in the Unreal asset. Complex queries use the
				// triangles unless the upload asked the shapes to answer them too.
				Body->AggGeom.EmptyElements();
				for (const FMantleStaticMeshBox& Box : Simple->Boxes)
				{
					FKBoxElem Element(Box.Size.X, Box.Size.Y, Box.Size.Z);
					Element.Center = FVector(Box.Center);
					Element.Rotation = FRotator(FQuat(Box.Rotation));
					Body->AggGeom.BoxElems.Add(Element);
				}
				for (const FMantleStaticMeshSphere& Sphere : Simple->Spheres)
				{
					FKSphereElem Element(Sphere.Radius);
					Element.Center = FVector(Sphere.Center);
					Body->AggGeom.SphereElems.Add(Element);
				}
				for (const FMantleStaticMeshCapsule& Capsule : Simple->Capsules)
				{
					FKSphylElem Element(Capsule.Radius, Capsule.Length);
					Element.Center = FVector(Capsule.Center);
					Element.Rotation = FRotator(FQuat(Capsule.Rotation));
					Body->AggGeom.SphylElems.Add(Element);
				}
				for (const FMantleStaticMeshHull& Hull : Simple->Hulls)
				{
					FKConvexElem Element;
					Element.VertexData.Reserve(Hull.Vertices.Num());
					for (const FVector3f& Point : Hull.Vertices)
					{
						Element.VertexData.Add(FVector(Point));
					}
					Element.UpdateElemBox();
					Body->AggGeom.ConvexElems.Add(MoveTemp(Element));
				}
				// Explicit, never CTF_UseDefault, which would defer to the project's DefaultShapeComplexity.
				Body->CollisionTraceFlag = Simple->bSimpleAsComplex ? CTF_UseSimpleAsComplex : CTF_UseSimpleAndComplex;
			}
			else
			{
				// The render triangles are the collision, from either side: a place wall must block a
				// player who walks into its back face as well as its front.
				Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			}
			// Any triangles cooked answer from either side, for the same reason.
			Body->bDoubleSidedGeometry = true;
			Body->InvalidatePhysicsData();
			if (Params.bCookCollisionNow)
			{
				Body->CreatePhysicsMeshes();
			}
		}
		return Built;
	}

	/**
	 * One MRM2 LOD into Description: one vertex instance per vertex, created in order, so the fast build's
	 * render vertex i is the LOD's vertex i and its index buffer comes out as the file's; one polygon group
	 * per section in draw order, named for its slot. Slots.SlotNames are LOD0's; every section must be one
	 * of them under that name, none twice. What names the LOD in messages.
	 */
	bool DescribeStaticLOD(const FMantleStaticMeshLOD& LOD, const TArray<FName>& SlotNames, const FString& What, FMeshDescription& Description,
		TMap<uint32, int32>& OutSlotByFirstIndex, int32& OutDropped, FString& OutError)
	{
		// The reader guarantees all of this; the builder checks again because it is public and a caller
		// could hand it anything.
		const int32 N = LOD.Positions.Num();
		if (N == 0 || LOD.Indices.Num() == 0 || LOD.Indices.Num() % 3 != 0 || LOD.Sections.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has %d vertices, %d indices and %d sections"), *What, N, LOD.Indices.Num(), LOD.Sections.Num());
			return false;
		}
		if (LOD.Normals.Num() != N || LOD.Tangents.Num() != N || LOD.UVs.Num() != N || (LOD.UV1s.Num() != 0 && LOD.UV1s.Num() != N)
			|| (LOD.Colors.Num() != 0 && LOD.Colors.Num() != N))
		{
			OutError = FString::Printf(TEXT("%s has %d vertices but %d normals, %d tangents, %d UVs, %d second UVs and %d colours"),
				*What, N, LOD.Normals.Num(), LOD.Tangents.Num(), LOD.UVs.Num(), LOD.UV1s.Num(), LOD.Colors.Num());
			return false;
		}
		const bool bUV1 = LOD.UV1s.Num() == N;
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			if (LOD.Positions[Vertex].ContainsNaN() || LOD.Normals[Vertex].ContainsNaN() || LOD.Tangents[Vertex].ContainsNaN() || LOD.UVs[Vertex].ContainsNaN()
				|| (bUV1 && LOD.UV1s[Vertex].ContainsNaN()))
			{
				OutError = FString::Printf(TEXT("%s's vertex %d is not finite"), *What, Vertex);
				return false;
			}
		}
		for (const uint32 Index : LOD.Indices)
		{
			if (Index >= static_cast<uint32>(N))
			{
				OutError = FString::Printf(TEXT("%s has an index of %u, out of range for %d vertices"), *What, Index, N);
				return false;
			}
		}
		TSet<int32> SlotsUsed;
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			bool bRepeated = false;
			SlotsUsed.Add(Section.MaterialIndex, &bRepeated);
			if (bRepeated || !SlotNames.IsValidIndex(Section.MaterialIndex) || Section.MaterialName.IsNone()
				|| !SlotNames[Section.MaterialIndex].ToString().Equals(Section.MaterialName.ToString(), ESearchCase::CaseSensitive)
				|| Section.FirstIndex < 0 || Section.IndexCount <= 0 || Section.FirstIndex % 3 != 0 || Section.IndexCount % 3 != 0
				|| static_cast<int64>(Section.FirstIndex) + Section.IndexCount > LOD.Indices.Num())
			{
				OutError = FString::Printf(TEXT("%s's section '%s' (slot %d, indices [%d, +%d)) is inconsistent"),
					*What, *Section.MaterialName.ToString(), Section.MaterialIndex, Section.FirstIndex, Section.IndexCount);
				return false;
			}
		}

		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		Description.ReserveNewVertices(N);
		Description.ReserveNewVertexInstances(N);
		Description.ReserveNewTriangles(LOD.Indices.Num() / 3);
		Description.ReserveNewPolygons(LOD.Indices.Num() / 3);
		Description.ReserveNewPolygonGroups(LOD.Sections.Num());

		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TPolygonGroupAttributesRef<FName> GroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		// UV1 is the render data's second texture coordinate channel (a material's TexCoord[1]).
		UVs.SetNumChannels(bUV1 ? 2 : 1);

		const bool bColors = LOD.Colors.Num() == N;
		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(N);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const FVertexID VertexID = Description.CreateVertex();
			Positions[VertexID] = LOD.Positions[Vertex];
			const FVertexInstanceID Instance = Description.CreateVertexInstance(VertexID);
			const FVector4f& Tangent = LOD.Tangents[Vertex];
			Normals[Instance] = LOD.Normals[Vertex];
			Tangents[Instance] = FVector3f(Tangent.X, Tangent.Y, Tangent.Z);
			BinormalSigns[Instance] = Tangent.W < 0.0f ? -1.0f : 1.0f;
			UVs.Set(Instance, 0, LOD.UVs[Vertex]);
			if (bUV1)
			{
				UVs.Set(Instance, 1, LOD.UV1s[Vertex]);
			}
			if (bColors)
			{
				// The fast build stores FLinearColor(attribute).ToFColor(true): the sRGB table here is its
				// inverse, so the vertex colour buffer holds the cooked bytes.
				Colors[Instance] = FVector4f(FLinearColor(LOD.Colors[Vertex]));
			}
			Instances.Add(Instance);
		}

		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			const FPolygonGroupID Group = Description.CreatePolygonGroup();
			GroupSlotNames[Group] = SlotNames[Section.MaterialIndex];
			const int32 TrianglesBefore = Description.Triangles().Num();
			const int32 End = Section.FirstIndex + Section.IndexCount;
			for (int32 First = Section.FirstIndex; First + 2 < End; First += 3)
			{
				const uint32 A = LOD.Indices[First];
				const uint32 B = LOD.Indices[First + 1];
				const uint32 C = LOD.Indices[First + 2];
				if (A == B || B == C || A == C)
				{
					++OutDropped;
					continue;
				}
				const FVertexInstanceID Corners[3] = { Instances[A], Instances[B], Instances[C] };
				Description.CreateTriangle(Group, MakeArrayView(Corners));
			}
			// The fast build makes a render section only for a group that kept a triangle.
			if (Description.Triangles().Num() > TrianglesBefore)
			{
				OutSlotByFirstIndex.Add(static_cast<uint32>(TrianglesBefore) * 3, Section.MaterialIndex);
			}
		}
		if (Description.Triangles().Num() == 0)
		{
			OutError = FString::Printf(TEXT("no triangle of %s names three different vertices"), *What);
			return false;
		}
		return true;
	}
}

bool FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources(const bool bRequested)
{
	// The engine's own condition (the check in FStaticMeshRenderData::InitResources): a process that can render,
	// or an uncooked one (the editor, a commandlet), which may initialise render resources as it always has.
	return bRequested && FApp::CanEverRenderOrProduceRenderData();
}

UStaticMesh* FMantleRuntimeStaticMeshBuilder::Build(const FMantleRuntimeMesh& Mesh, const FMantleStaticMeshBuildParams& Params, FString* OutError)
{
	using namespace MantleRuntimeStaticMeshBuilderPrivate;

	if (!IsInGameThread())
	{
		return FailStaticMeshBuild(OutError, TEXT("static meshes are built on the game thread"));
	}
	const int32 N = Mesh.Positions.Num();
	const bool bNormals = Mesh.Normals.Num() > 0;
	const bool bUVs = Mesh.UVs.Num() > 0;
	if (N == 0 || Mesh.Indices.Num() == 0 || Mesh.Indices.Num() % 3 != 0)
	{
		return FailStaticMeshBuild(OutError, FString::Printf(TEXT("the mesh has %d vertices and %d indices"), N, Mesh.Indices.Num()));
	}
	if ((bNormals && Mesh.Normals.Num() != N) || (bUVs && Mesh.UVs.Num() != N))
	{
		return FailStaticMeshBuild(OutError, FString::Printf(TEXT("the mesh has %d vertices, %d normals and %d UVs"), N, Mesh.Normals.Num(), Mesh.UVs.Num()));
	}
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		if (Mesh.Positions[Vertex].ContainsNaN() || (bNormals && Mesh.Normals[Vertex].ContainsNaN()) || (bUVs && Mesh.UVs[Vertex].ContainsNaN()))
		{
			return FailStaticMeshBuild(OutError, FString::Printf(TEXT("vertex %d is not finite"), Vertex));
		}
	}
	for (const uint32 Index : Mesh.Indices)
	{
		if (Index >= static_cast<uint32>(N))
		{
			return FailStaticMeshBuild(OutError, FString::Printf(TEXT("an index is %u, out of range for %d vertices"), Index, N));
		}
	}

	// The mesh description: one vertex instance per vertex (MRM1 vertices are already split where
	// normals or UVs differ), one polygon group for the one section.
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	Description.ReserveNewVertices(N);
	Description.ReserveNewVertexInstances(N);
	Description.ReserveNewTriangles(Mesh.Indices.Num() / 3);
	Description.ReserveNewPolygons(Mesh.Indices.Num() / 3);

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	SlotNames[Group] = Params.MaterialSlotName;

	TArray<FVertexInstanceID> Instances;
	Instances.Reserve(N);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const FVertexID VertexID = Description.CreateVertex();
		Positions[VertexID] = Mesh.Positions[Vertex];
		const FVertexInstanceID Instance = Description.CreateVertexInstance(VertexID);
		Normals[Instance] = bNormals ? Mesh.Normals[Vertex] : FVector3f::ZeroVector;
		UVs.Set(Instance, 0, bUVs ? Mesh.UVs[Vertex] : FVector2f::ZeroVector);
		Instances.Add(Instance);
	}

	int32 Dropped = 0;
	for (int32 First = 0; First + 2 < Mesh.Indices.Num(); First += 3)
	{
		const uint32 A = Mesh.Indices[First];
		const uint32 B = Mesh.Indices[First + 1];
		const uint32 C = Mesh.Indices[First + 2];
		if (A == B || B == C || A == C)
		{
			++Dropped;
			continue;
		}
		const FVertexInstanceID Corners[3] = { Instances[A], Instances[B], Instances[C] };
		Description.CreateTriangle(Group, MakeArrayView(Corners));
	}
	if (Description.Triangles().Num() == 0)
	{
		return FailStaticMeshBuild(OutError, TEXT("no triangle names three different vertices"));
	}

	// Tangents from the UVs always (MRM1 has none); normals too when the upload carried none.
	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Description);
	EComputeNTBsFlags Flags = EComputeNTBsFlags::Tangents | EComputeNTBsFlags::WeightedNTBs;
	if (!bNormals)
	{
		Flags |= EComputeNTBsFlags::Normals;
	}
	FStaticMeshOperations::ComputeTangentsAndNormals(Description, Flags);

	FSlotSpec Slots;
	Slots.SlotNames.Add(Params.MaterialSlotName);
	Slots.SlotByFirstIndex.AddDefaulted_GetRef().Add(0, 0);
	const TArray<FMeshDescription*> Descriptions = { &Description };
	UStaticMesh* Built = FinishStaticMeshBuild(Descriptions, Slots, TConstArrayView<float>(), nullptr, Params, OutError);
	if (Built)
	{
		UE_LOG(LogMantle, Verbose, TEXT("MANTLE_STATICMESH built %s: %d vertices, %d triangles (%d degenerate dropped)"),
			*Built->GetName(), N, Description.Triangles().Num(), Dropped);
	}
	return Built;
}

UStaticMesh* FMantleRuntimeStaticMeshBuilder::Build(const FMantleStaticMeshData& Mesh, const FMantleStaticMeshBuildParams& Params, FString* OutError)
{
	using namespace MantleRuntimeStaticMeshBuilderPrivate;

	if (!IsInGameThread())
	{
		return FailStaticMeshBuild(OutError, TEXT("static meshes are built on the game thread"));
	}

	// LOD0's sections name the slots: each of 0..count-1 exactly once, named for its material.
	const int32 SectionCount = Mesh.Sections.Num();
	FSlotSpec Slots;
	Slots.SlotNames.Init(NAME_None, SectionCount);
	for (const FMantleSkinnedMeshSection& Section : Mesh.Sections)
	{
		if (Section.MaterialIndex < 0 || Section.MaterialIndex >= SectionCount || !Slots.SlotNames[Section.MaterialIndex].IsNone() || Section.MaterialName.IsNone())
		{
			return FailStaticMeshBuild(OutError, FString::Printf(TEXT("section '%s' (slot %d, indices [%d, +%d)) is inconsistent"),
				*Section.MaterialName.ToString(), Section.MaterialIndex, Section.FirstIndex, Section.IndexCount));
		}
		Slots.SlotNames[Section.MaterialIndex] = Section.MaterialName;
	}

	// The lower LODs when asked for and when their screen sizes are there to switch at.
	const int32 LODCount = Params.bBuildLowerLODs && Mesh.ScreenSizes.Num() == Mesh.LODCount() ? Mesh.LODCount() : 1;
	TArray<FMeshDescription> Descriptions;
	Descriptions.SetNum(LODCount);
	int32 Dropped = 0;
	int32 Triangles = 0;
	for (int32 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
	{
		FString Error;
		const FString What = LODIndex == 0 ? FString(TEXT("the mesh")) : FString::Printf(TEXT("LOD%d"), LODIndex);
		if (!DescribeStaticLOD(Mesh.GetLOD(LODIndex), Slots.SlotNames, What, Descriptions[LODIndex], Slots.SlotByFirstIndex.AddDefaulted_GetRef(), Dropped, Error))
		{
			return FailStaticMeshBuild(OutError, Error);
		}
		Triangles += Descriptions[LODIndex].Triangles().Num();
	}

	TArray<FMeshDescription*> DescriptionPointers;
	for (FMeshDescription& Description : Descriptions)
	{
		DescriptionPointers.Add(&Description);
	}
	UStaticMesh* Built = FinishStaticMeshBuild(DescriptionPointers, Slots, Mesh.ScreenSizes, &Mesh.Collision, Params, OutError);
	if (Built)
	{
		UE_LOG(LogMantle, Verbose, TEXT("MANTLE_STATICMESH built %s: %d vertices, %d triangles over %d LOD(s), %d sections, %d simple collision shape(s) (%d degenerate dropped)"),
			*Built->GetName(), Mesh.VertexCount(), Triangles, LODCount, SectionCount, Mesh.Collision.ShapeCount(), Dropped);
	}
	return Built;
}
