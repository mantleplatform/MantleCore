// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisSkin.h"

#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleFit.h"
#include "MantleVehiclePreset.h"

#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "MeshUVChannelInfo.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Engine/StaticMesh.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisSkinPrivate
{
	/** The model's bones in the model's space: each bone's local pose put on its parent's (the raw skeleton lists parents first). */
	TArray<FTransform> ComponentTransforms(const FMantleSkinnedMesh& Skinned)
	{
		TArray<FTransform> Component;
		Component.SetNum(Skinned.Bones.Num());
		for (int32 Bone = 0; Bone < Skinned.Bones.Num(); ++Bone)
		{
			const FTransform Local = Skinned.ReferencePose.IsValidIndex(Bone) ? Skinned.ReferencePose[Bone] : FTransform::Identity;
			const int32 Parent = Skinned.Bones[Bone].ParentIndex;
			Component[Bone] = Parent >= 0 && Parent < Bone ? Local * Component[Parent] : Local;
		}
		return Component;
	}

	TArray<FVector> ComponentPositions(const FMantleSkinnedMesh& Skinned)
	{
		const TArray<FTransform> Component = ComponentTransforms(Skinned);
		TArray<FVector> Positions;
		Positions.SetNum(Component.Num());
		for (int32 Bone = 0; Bone < Component.Num(); ++Bone)
		{
			Positions[Bone] = Component[Bone].GetLocation();
		}
		return Positions;
	}

	/**
	 * The model's UV channels above 0 (up to the skeletal mesh's limit of MAX_TEXCOORDS channels in all) and its painted vertex colours, from the vertex buffers of the LOD
	 * whose vertices are already in Out.LOD. A colour buffer that is every white means no colours at all, as the upload takes it.
	 */
	void ReadExtras(const FStaticMeshVertexBuffer& Vertices, const FColorVertexBuffer& Colors, const bool bHasColors, FMantleVehicleModel& Out)
	{
		const uint32 NumVertices = static_cast<uint32>(Out.LOD.Positions.Num());
		if (NumVertices == 0)
		{
			return;
		}
		if (Vertices.GetNumVertices() == NumVertices && Vertices.GetTexCoordData() != nullptr)
		{
			const uint32 Channels = FMath::Min<uint32>(Vertices.GetNumTexCoords(), MAX_TEXCOORDS);
			for (uint32 Channel = 1; Channel < Channels; ++Channel)
			{
				TArray<FVector2f>& Uvs = Out.ExtraUVs.AddDefaulted_GetRef();
				Uvs.SetNumUninitialized(NumVertices);
				for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
				{
					Uvs[Vertex] = Vertices.GetVertexUV(Vertex, Channel);
				}
			}
		}
		if (bHasColors && Colors.GetNumVertices() == NumVertices && Colors.GetVertexData() != nullptr)
		{
			Out.Colors.SetNumUninitialized(NumVertices);
			bool bAllWhite = true;
			for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			{
				Out.Colors[Vertex] = Colors.VertexColor(Vertex);
				bAllWhite = bAllWhite && Out.Colors[Vertex] == FColor::White;
			}
			if (bAllWhite)
			{
				Out.Colors.Reset();
			}
		}
	}

	/** The boxes around the bodies of an aggregate geometry (boxes, spheres, capsules, convex hulls), with BoneTM the bone they hang on: a turned shape is widened to the box around it. */
	void AddAggregate(const FKAggregateGeom& Geom, const FTransform& BoneTM, const int32 Bone, TArray<FMantleVehicleModelBody>& Out)
	{
		const auto Add = [&Out, Bone](const FBox& Box)
		{
			if (Box.IsValid != 0 && !Box.Min.ContainsNaN() && !Box.Max.ContainsNaN())
			{
				FMantleVehicleModelBody& Body = Out.AddDefaulted_GetRef();
				Body.Box = Box;
				Body.Bone = Bone;
			}
		};
		for (const FKBoxElem& Element : Geom.BoxElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f));
		}
		for (const FKSphereElem& Element : Geom.SphereElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f));
		}
		for (const FKSphylElem& Element : Geom.SphylElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f));
		}
		for (const FKTaperedCapsuleElem& Element : Geom.TaperedCapsuleElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f));
		}
		for (const FKConvexElem& Element : Geom.ConvexElems)
		{
			Add(Element.CalcAABB(BoneTM, FVector::OneVector));
		}
	}

	bool ReadSkeletal(USkeletalMesh* Mesh, FMantleVehicleModel& Out, FString& OutError)
	{
		TArray<USkinnedAsset*> Compiling = { Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		// A mesh with nothing built to read (a new empty one) is said so here, quietly: the extraction below logs an error for it.
		const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no geometry to read."), *Mesh->GetName());
			return false;
		}
		FMantleSkinnedMesh Skinned;
		FString Why;
		if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Mesh, Skinned, &Why, &Out.Materials) || Skinned.LODs.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s cannot be read here (%s). Open it in the editor once and save it, then try again."), *Mesh->GetName(), *Why);
			return false;
		}
		Out.bSkeletal = true;
		Out.LOD = MoveTemp(Skinned.LODs[0]);
		Out.Bones = Skinned.Bones;
		const TArray<FTransform> Pose = ComponentTransforms(Skinned);
		Out.BonePositions = ComponentPositions(Skinned);

		// The model's other UV channels and vertex colours (the extraction above keeps the first UV set only), and its physics asset's bodies: what its collision is shaped like.
		const FSkeletalMeshLODRenderData& SourceLOD = RenderData->LODRenderData[0];
		ReadExtras(SourceLOD.StaticVertexBuffers.StaticMeshVertexBuffer, SourceLOD.StaticVertexBuffers.ColorVertexBuffer, /*bHasColors*/ true, Out);
		if (const UPhysicsAsset* Physics = Mesh->GetPhysicsAsset())
		{
			for (const USkeletalBodySetup* Setup : Physics->SkeletalBodySetups)
			{
				const FName BoneName = Setup ? Setup->BoneName : NAME_None;
				const int32 Bone = Skinned.Bones.IndexOfByPredicate([BoneName](const FMantleSkinnedMeshBone& Candidate) { return Candidate.Name == BoneName; });
				if (Bone != INDEX_NONE && Pose.IsValidIndex(Bone))
				{
					AddAggregate(Setup->AggGeom, Pose[Bone], Bone, Out.PhysicsBodies);
				}
			}
		}
		return true;
	}

	/** The material slots of a static mesh as the body's: a skeletal material for each. */
	void AddStaticMaterials(const UStaticMesh& Mesh, FMantleVehicleModel& Out)
	{
		for (const FStaticMaterial& Material : Mesh.GetStaticMaterials())
		{
			Out.Materials.Add(FSkeletalMaterial(Material.MaterialInterface, Material.MaterialSlotName));
		}
	}

	/** A static mesh's own simple collision (the shapes in its body setup), as boxes around them. */
	void AddStaticPhysics(const UStaticMesh& Mesh, FMantleVehicleModel& Out)
	{
		if (const UBodySetup* Setup = Mesh.GetBodySetup())
		{
			AddAggregate(Setup->AggGeom, FTransform::Identity, INDEX_NONE, Out.PhysicsBodies);
		}
	}

	/**
	 * A Nanite static mesh read from its full-detail source (its mesh description), and not its render LOD 0, which is Nanite's reduced fallback. True when it was read that
	 * way; Out is then the whole model. False leaves Out alone but for a note saying why the fallback is used (the source is not kept in this project, or has more
	 * triangles than a vehicle may carry). A mesh that does not use Nanite is not touched.
	 */
	bool ReadStaticNanite(UStaticMesh* Mesh, const int32 FallbackTriangles, FMantleVehicleModel& Out)
	{
		if (!Mesh->IsNaniteEnabled())
		{
			return false;
		}
		FMeshDescription Description;
		const bool bSource = Mesh->IsMeshDescriptionValid(0) && Mesh->CloneMeshDescription(0, Description);
		const int32 SourceTriangles = bSource ? Description.Triangles().Num() : 0;
		FString Note;
		const bool bUse = FMantleVehicleModel::UseNaniteSource(true, bSource, SourceTriangles, FallbackTriangles, &Note);
		if (!Note.IsEmpty())
		{
			Out.Notes.Add(Note);
		}
		if (!bUse)
		{
			return false;
		}

		const FStaticMeshConstAttributes Attributes(Description);
		const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		const TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		const TVertexInstanceAttributesConstRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		const TVertexInstanceAttributesConstRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		const TPolygonGroupAttributesConstRef<FName> GroupNames = Attributes.GetPolygonGroupMaterialSlotNames();
		const int32 NumChannels = FMath::Min<int32>(UVs.IsValid() ? UVs.GetNumChannels() : 0, MAX_TEXCOORDS);
		const bool bColors = Colors.IsValid();

		FMantleSkinnedMeshLOD& LOD = Out.LOD;
		TArray<int32> IndexOfInstance;
		IndexOfInstance.Init(INDEX_NONE, Description.VertexInstances().GetArraySize());
		for (int32 Channel = 1; Channel < NumChannels; ++Channel)
		{
			Out.ExtraUVs.AddDefaulted();
		}
		// A vertex for each vertex instance a triangle uses, in the order the triangles meet them.
		const auto VertexOf = [&](const FVertexInstanceID Instance) -> uint32
		{
			int32& Slot = IndexOfInstance[Instance.GetValue()];
			if (Slot == INDEX_NONE)
			{
				Slot = LOD.Positions.Num();
				LOD.Positions.Add(Positions[Description.GetVertexInstanceVertex(Instance)]);
				LOD.Normals.Add(Normals[Instance]);
				const FVector3f Tangent = Tangents[Instance];
				LOD.Tangents.Add(FVector4f(Tangent.X, Tangent.Y, Tangent.Z, Signs[Instance] < 0.0f ? -1.0f : 1.0f));
				LOD.UVs.Add(NumChannels > 0 ? UVs.Get(Instance, 0) : FVector2f::ZeroVector);
				for (int32 Channel = 1; Channel < NumChannels; ++Channel)
				{
					Out.ExtraUVs[Channel - 1].Add(UVs.Get(Instance, Channel));
				}
				if (bColors)
				{
					const FVector4f Color = Colors[Instance];
					Out.Colors.Add(FLinearColor(Color.X, Color.Y, Color.Z, Color.W).ToFColor(true));
				}
			}
			return static_cast<uint32>(Slot);
		};

		AddStaticMaterials(*Mesh, Out);
		const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
		for (const FPolygonGroupID GroupID : Description.PolygonGroups().GetElementIDs())
		{
			const TArrayView<const FTriangleID> Triangles = Description.GetPolygonGroupTriangles(GroupID);
			if (Triangles.Num() == 0)
			{
				continue;
			}
			const FName SlotName = GroupNames.IsValid() ? GroupNames[GroupID] : FName();
			int32 Slot = StaticMaterials.IndexOfByPredicate([SlotName](const FStaticMaterial& Material)
			{
				return !SlotName.IsNone() && (Material.ImportedMaterialSlotName == SlotName || Material.MaterialSlotName == SlotName);
			});
			if (Slot == INDEX_NONE)
			{
				Slot = FMath::Clamp(GroupID.GetValue(), 0, FMath::Max(StaticMaterials.Num() - 1, 0));
			}
			FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
			Section.MaterialIndex = Slot;
			Section.FirstIndex = LOD.Indices.Num();
			Section.IndexCount = Triangles.Num() * 3;
			if (StaticMaterials.IsValidIndex(Slot))
			{
				Section.MaterialName = StaticMaterials[Slot].MaterialSlotName;
			}
			for (const FTriangleID TriangleID : Triangles)
			{
				for (const FVertexInstanceID Instance : Description.GetTriangleVertexInstances(TriangleID))
				{
					LOD.Indices.Add(VertexOf(Instance));
				}
			}
		}
		LOD.NumInfluences = 1;
		// An all-white colour list is no colours.
		if (Out.Colors.Num() > 0 && !Out.Colors.ContainsByPredicate([](const FColor& Color) { return Color != FColor::White; }))
		{
			Out.Colors.Reset();
		}
		return LOD.Indices.Num() >= 3;
	}

	bool ReadStatic(UStaticMesh* Mesh, FMantleVehicleModel& Out, FString& OutError)
	{
		TArray<UStaticMesh*> Compiling = { Mesh };
		FStaticMeshCompilingManager::Get().FinishCompilation(Compiling);
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		// A Nanite mesh's render LOD 0 is its reduced fallback: its full-detail source is read when that is kept and fits a vehicle.
		const int32 FallbackTriangles = RenderData && RenderData->LODResources.Num() > 0 ? RenderData->LODResources[0].GetNumTriangles() : 0;
		if (ReadStaticNanite(Mesh, FallbackTriangles, Out))
		{
			AddStaticPhysics(*Mesh, Out);
			return true;
		}
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no geometry to read."), *Mesh->GetName());
			return false;
		}
		const FStaticMeshLODResources& Resources = RenderData->LODResources[0];
		const FPositionVertexBuffer& Positions = Resources.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& Vertices = Resources.VertexBuffers.StaticMeshVertexBuffer;
		const uint32 NumVertices = Positions.GetNumVertices();
		const FIndexArrayView Indices = Resources.IndexBuffer.GetArrayView();
		if (NumVertices == 0 || Positions.GetVertexData() == nullptr || Vertices.GetNumVertices() != NumVertices || Vertices.GetTangentData() == nullptr
			|| Vertices.GetNumTexCoords() == 0 || Vertices.GetTexCoordData() == nullptr || Indices.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s cannot be read here: its geometry is not kept in memory. Tick Allow CPU Access in its settings, save it, and try again."), *Mesh->GetName());
			return false;
		}

		FMantleSkinnedMeshLOD& LOD = Out.LOD;
		LOD.Positions.SetNumUninitialized(NumVertices);
		LOD.Normals.SetNumUninitialized(NumVertices);
		LOD.Tangents.SetNumUninitialized(NumVertices);
		LOD.UVs.SetNumUninitialized(NumVertices);
		for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			LOD.Positions[Vertex] = Positions.VertexPosition(Vertex);
			const FVector4f TangentX = Vertices.VertexTangentX(Vertex);
			const FVector4f TangentZ = Vertices.VertexTangentZ(Vertex);
			LOD.Normals[Vertex] = FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z);
			LOD.Tangents[Vertex] = FVector4f(TangentX.X, TangentX.Y, TangentX.Z, TangentZ.W < 0.0f ? -1.0f : 1.0f);
			LOD.UVs[Vertex] = Vertices.GetVertexUV(Vertex, 0);
		}
		// Its other UV channels and painted vertex colours go onto the body with its own tangents.
		ReadExtras(Vertices, Resources.VertexBuffers.ColorVertexBuffer, Resources.bHasColorVertexData, Out);

		const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
		AddStaticMaterials(*Mesh, Out);
		for (const FStaticMeshSection& Section : Resources.Sections)
		{
			FMantleSkinnedMeshSection& Added = LOD.Sections.AddDefaulted_GetRef();
			Added.MaterialIndex = Section.MaterialIndex;
			Added.IndexCount = static_cast<int32>(Section.NumTriangles) * 3;
			if (StaticMaterials.IsValidIndex(Section.MaterialIndex))
			{
				Added.MaterialName = StaticMaterials[Section.MaterialIndex].MaterialSlotName;
			}
			for (uint32 Index = 0; Index < Section.NumTriangles * 3; ++Index)
			{
				const uint32 At = Section.FirstIndex + Index;
				if (static_cast<int32>(At) >= Indices.Num())
				{
					break;
				}
				LOD.Indices.Add(Indices[At]);
			}
			// The section's range in the index list this model keeps (only the sections' own triangles are kept, in section order).
			Added.FirstIndex = LOD.Indices.Num() - Added.IndexCount;
		}
		LOD.NumInfluences = 1;
		AddStaticPhysics(*Mesh, Out);
		return LOD.Indices.Num() >= 3;
	}

	/** Which model bone each vertex is weighted to most; INDEX_NONE for a static model. */
	TArray<int32> HeaviestBones(const FMantleVehicleModel& Model)
	{
		const FMantleSkinnedMeshLOD& LOD = Model.LOD;
		TArray<int32> Out;
		Out.Init(INDEX_NONE, LOD.Positions.Num());
		if (!Model.bSkeletal || LOD.NumInfluences < 1)
		{
			return Out;
		}
		for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
		{
			int32 Best = INDEX_NONE;
			int32 BestWeight = -1;
			for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
			{
				const int32 At = Vertex * LOD.NumInfluences + Slot;
				if (LOD.InfluenceWeights.IsValidIndex(At) && static_cast<int32>(LOD.InfluenceWeights[At]) > BestWeight)
				{
					BestWeight = LOD.InfluenceWeights[At];
					Best = LOD.InfluenceBones[At];
				}
			}
			Out[Vertex] = Best;
		}
		return Out;
	}

	/** True when Bone is Ancestor or lies under it. */
	bool IsUnder(const FMantleVehicleModel& Model, int32 Bone, const int32 Ancestor)
	{
		for (int32 Guard = 0; Bone != INDEX_NONE && Guard < 1024; ++Guard)
		{
			if (Bone == Ancestor)
			{
				return true;
			}
			Bone = Model.Bones.IsValidIndex(Bone) ? Model.Bones[Bone].ParentIndex : INDEX_NONE;
		}
		return false;
	}

	/** The vertices a piece of the model stands for, by the whole-vertex rule the skin uses (the heaviest influence under the bone; every vertex of the section's triangles). */
	void PieceVertices(const FMantleVehicleModel& Model, const TArray<int32>& Heaviest, const FMantleVehicleModelPiece& Piece, TArray<int32>& Out)
	{
		Out.Reset();
		if (Piece.Kind == FMantleVehicleModelPiece::EKind::Bone)
		{
			const int32 Bone = Model.FindBone(Piece.Bone);
			if (Bone == INDEX_NONE)
			{
				return;
			}
			for (int32 Vertex = 0; Vertex < Heaviest.Num(); ++Vertex)
			{
				if (IsUnder(Model, Heaviest[Vertex], Bone))
				{
					Out.Add(Vertex);
				}
			}
		}
		else if (Piece.Kind == FMantleVehicleModelPiece::EKind::Section && Model.LOD.Sections.IsValidIndex(Piece.Section))
		{
			const FMantleSkinnedMeshSection& Section = Model.LOD.Sections[Piece.Section];
			TSet<int32> Seen;
			for (int32 At = Section.FirstIndex; At < Section.FirstIndex + Section.IndexCount && At < Model.LOD.Indices.Num(); ++At)
			{
				bool bAlready = false;
				Seen.Add(static_cast<int32>(Model.LOD.Indices[At]), &bAlready);
				if (!bAlready)
				{
					Out.Add(static_cast<int32>(Model.LOD.Indices[At]));
				}
			}
		}
	}

	FBox PointsBox(const FMantleVehicleModel& Model, const TArray<int32>& Vertices, const FMantleVehicleModelPlacement& Placement)
	{
		FBox Box(ForceInit);
		for (const int32 Vertex : Vertices)
		{
			Box += Placement.Point(FVector(Model.LOD.Positions[Vertex]));
		}
		return Box;
	}

	/** A chassis bone's name as the window words it. */
	FText LabelOf(const FString& Bone, const FMantleChassisSpec& Spec)
	{
		const bool bPlane = Spec.Aircraft.bSet;
		if (Bone == TEXT("wheel_f"))
		{
			return FText::FromString(bPlane ? TEXT("Nose wheel") : TEXT("Front wheel"));
		}
		if (Bone == TEXT("wheel_r"))
		{
			return FText::FromString(TEXT("Rear wheel"));
		}
		if (Bone == TEXT("wheel_fl"))
		{
			return FText::FromString(TEXT("Front left wheel"));
		}
		if (Bone == TEXT("wheel_fr"))
		{
			return FText::FromString(TEXT("Front right wheel"));
		}
		if (Bone == TEXT("wheel_rl"))
		{
			return FText::FromString(bPlane ? TEXT("Left main wheel") : TEXT("Rear left wheel"));
		}
		if (Bone == TEXT("wheel_rr"))
		{
			return FText::FromString(bPlane ? TEXT("Right main wheel") : TEXT("Rear right wheel"));
		}
		if (Bone.StartsWith(TEXT("wheel_a")))
		{
			// wheel_a1_l: the front axle's left wheel; a three or four axle chassis.
			const FString Rest = Bone.RightChop(7);
			FString Axle;
			FString Side;
			if (Rest.Split(TEXT("_"), &Axle, &Side))
			{
				return FText::FromString(FString::Printf(TEXT("%s wheel, axle %s"), Side == TEXT("l") ? TEXT("Left") : TEXT("Right"), *Axle));
			}
		}
		if (Bone.StartsWith(TEXT("fork")))
		{
			return FText::FromString(TEXT("Front fork (turns when you steer)"));
		}
		if (Bone == TEXT("aileron_l"))
		{
			return FText::FromString(TEXT("Left aileron (flap on the back of the left wing)"));
		}
		if (Bone == TEXT("aileron_r"))
		{
			return FText::FromString(TEXT("Right aileron (flap on the back of the right wing)"));
		}
		if (Bone == TEXT("elevator"))
		{
			return FText::FromString(TEXT("Elevator (the tailplane's flap)"));
		}
		if (Bone == TEXT("rudder"))
		{
			return FText::FromString(TEXT("Rudder (the tail fin's flap)"));
		}
		if (Bone == TEXT("propeller"))
		{
			return FText::FromString(TEXT("Propeller"));
		}
		if (Bone == TEXT("propeller_l"))
		{
			return FText::FromString(TEXT("Left propeller"));
		}
		if (Bone == TEXT("propeller_r"))
		{
			return FText::FromString(TEXT("Right propeller"));
		}
		if (Bone == TEXT("main_rotor"))
		{
			return FText::FromString(TEXT("Main rotor (the big blades on top)"));
		}
		if (Bone == TEXT("tail_rotor"))
		{
			return FText::FromString(TEXT("Tail rotor (the small blades on the tail)"));
		}
		return FText::FromString(Bone);
	}

	FText HelpOf(const EMantleVehiclePartKind Kind)
	{
		switch (Kind)
		{
		case EMantleVehiclePartKind::Wheel:
			return FText::FromString(TEXT("Spins as the vehicle rolls."));
		case EMantleVehiclePartKind::Fork:
			return FText::FromString(TEXT("Turns the front wheel with the steering."));
		case EMantleVehiclePartKind::Spinner:
			return FText::FromString(TEXT("Spins while the vehicle has a driver."));
		default:
			return FText::FromString(TEXT("Tilts as you steer the aircraft."));
		}
	}

	bool Has(const FString& Lower, const TCHAR* Word)
	{
		return Lower.Contains(Word, ESearchCase::CaseSensitive);
	}

	/** True for a lower-cased bone name that is a propeller's (a "prop" that is not a guard). Counting and suggesting share it, so they agree on what a propeller is. */
	bool IsPropellerName(const FString& Lower)
	{
		return Has(Lower, TEXT("prop")) && !Has(Lower, TEXT("guard"));
	}

	/**
	 * The model's bones (never the first, the skeleton's root) whose lower-cased name passes Test, in the order they are listed. A part rigged with a chain of bones
	 * (MainRotor1, MainRotor, MainRotor_end) is one part: of the bones that pass, the one nearest the root stands for all, because the ones below it are carried with it.
	 */
	TArray<int32> TopBones(const FMantleVehicleModel& Model, const TFunctionRef<bool(const FString&)> Test)
	{
		TArray<int32> Found;
		for (int32 Bone = 1; Bone < Model.Bones.Num(); ++Bone)
		{
			if (Test(Model.Bones[Bone].Name.ToString().ToLower()))
			{
				Found.Add(Bone);
			}
		}
		TArray<int32> Top;
		for (const int32 Bone : Found)
		{
			const bool bCarried = Found.ContainsByPredicate([&Model, Bone](const int32 Other) { return Other != Bone && IsUnder(Model, Bone, Other); });
			if (!bCarried)
			{
				Top.Add(Bone);
			}
		}
		return Top;
	}

	/** What the build says about a model too small to measure (under a centimetre across): it is left as modelled, which is usually far too small or too big. */
	FString TooSmallNote()
	{
		return TEXT("This model is under 1 cm across, so Mantle can't measure it to fit it to the vehicle and leaves it at the size it was modelled. It was probably modelled in metres or exported with the wrong units: rescale it in your 3D program, export it again and pick it again.");
	}

	/** Scale limits the placement holds a model to: a model further off than this from the vehicle's size is not scaled all the way. */
	constexpr double MinModelScale = 0.05;
	constexpr double MaxModelScale = 20.0;

	/**
	 * The scale a model is placed at when its size asks for Wanted: held to the limits above. When it is held, Note says so in plain words (what is probably wrong with
	 * the model's units, and how far short the result falls), so the creator is told and not left with a vehicle that looks wrong for no reason.
	 */
	double HeldModelScale(const double Wanted, FString& Note)
	{
		const double Used = FMath::Clamp(Wanted, MinModelScale, MaxModelScale);
		if (Used != Wanted)
		{
			const bool bTooSmall = Wanted > MaxModelScale;
			const double Times = bTooSmall ? Wanted : 1.0 / FMath::Max(Wanted, KINDA_SMALL_NUMBER);
			Note = FString::Printf(TEXT("This model is about %.0f times %s than this vehicle%s. Mantle %s it by the most it will, %.2f times, so it is still %s than the vehicle. Rescale it in your 3D program, export it again and pick it again."),
				Times, bTooSmall ? TEXT("smaller") : TEXT("bigger"),
				bTooSmall ? (Times >= 50.0 ? TEXT(" (it looks like it was modelled in metres, not centimetres)") : TEXT("")) : (Times >= 50.0 ? TEXT(" (it looks like it was modelled in millimetres)") : TEXT("")),
				bTooSmall ? TEXT("scaled it up") : TEXT("scaled it down"), Used, bTooSmall ? TEXT("small") : TEXT("big"));
		}
		return Used;
	}

	/** Ends in l or r as a word of its own ("aileron_l", "propLeft"): -1 left, +1 right, 0 neither. */
	int32 SideOf(const FString& Lower)
	{
		if (Has(Lower, TEXT("left")) || Lower.EndsWith(TEXT("_l")) || Lower.EndsWith(TEXT(".l")) || Lower.EndsWith(TEXT("-l")))
		{
			return -1;
		}
		if (Has(Lower, TEXT("right")) || Lower.EndsWith(TEXT("_r")) || Lower.EndsWith(TEXT(".r")) || Lower.EndsWith(TEXT("-r")))
		{
			return 1;
		}
		return 0;
	}

	/** Whether a wheel piece name is on a front axle (a car's wheel_f, wheel_fl, wheel_fr or a bike's first axle). */
	bool IsFrontWheelName(const FString& Name)
	{
		return Name == TEXT("wheel_f") || Name == TEXT("wheel_fl") || Name == TEXT("wheel_fr") || Name == TEXT("wheel_a1_l") || Name == TEXT("wheel_a1_r");
	}
}

FBox FMantleVehicleModel::Bounds() const
{
	FBox Box(ForceInit);
	for (const FVector3f& Position : LOD.Positions)
	{
		Box += FVector(Position);
	}
	return Box;
}

int32 FMantleVehicleModel::FindBone(const FName Bone) const
{
	return Bones.IndexOfByPredicate([Bone](const FMantleSkinnedMeshBone& Candidate) { return Candidate.Name == Bone; });
}

bool FMantleVehicleModel::UseNaniteSource(const bool bNanite, const bool bSourceAvailable, const int32 SourceTriangles, const int32 FallbackTriangles, FString* OutNote)
{
	if (OutNote)
	{
		OutNote->Reset();
	}
	if (!bNanite)
	{
		return false;
	}
	const FString Fallback = FallbackTriangles > 0 ? FString::Printf(TEXT(" (%d triangles)"), FallbackTriangles) : FString();
	if (!bSourceAvailable || SourceTriangles <= 0)
	{
		if (OutNote)
		{
			*OutNote = FString::Printf(TEXT("Your model uses Nanite and its full-detail source isn't kept in this project, so the vehicle is made from Nanite's reduced fallback mesh%s. It may look coarser than your model does in the editor; re-import the model with its source kept to get all its detail."), *Fallback);
		}
		return false;
	}
	if (SourceTriangles > FMantleVehicleLimits::MaxTriangles)
	{
		if (OutNote)
		{
			*OutNote = FString::Printf(TEXT("Your model uses Nanite and its full detail (%d triangles) is more than a vehicle can carry (%d), so the vehicle is made from Nanite's reduced fallback mesh%s. To keep more detail, raise Fallback Triangle Percent in the mesh's Nanite settings."),
				SourceTriangles, FMantleVehicleLimits::MaxTriangles, *Fallback);
		}
		return false;
	}
	if (OutNote)
	{
		*OutNote = FString::Printf(TEXT("Your model uses Nanite, so the vehicle is made from its full-detail source (%d triangles) and not Nanite's reduced fallback mesh%s."), SourceTriangles, *Fallback);
	}
	return true;
}

bool FMantleVehicleModel::Read(UObject* Asset, FMantleVehicleModel& Out, FString& OutError)
{
	using namespace MantleVehicleChassisSkinPrivate;
	Out = FMantleVehicleModel();
	if (!Asset)
	{
		OutError = TEXT("Pick the model to use as the vehicle's body.");
		return false;
	}
	Out.Name = Asset->GetName();
	bool bRead = false;
	if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Asset))
	{
		bRead = ReadSkeletal(Skeletal, Out, OutError);
	}
	else if (UStaticMesh* Static = Cast<UStaticMesh>(Asset))
	{
		bRead = ReadStatic(Static, Out, OutError);
	}
	else
	{
		OutError = FString::Printf(TEXT("%s isn't a model. Pick a Static Mesh or a Skeletal Mesh."), *Asset->GetName());
		return false;
	}
	if (!bRead)
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s has no geometry to read."), *Asset->GetName());
		}
		return false;
	}
	if (!Out.Bounds().IsValid)
	{
		OutError = FString::Printf(TEXT("%s has no geometry to read."), *Asset->GetName());
		return false;
	}
	return true;
}

FString FMantleVehicleModel::Describe() const
{
	return FString::Printf(TEXT("%s%s, %d triangles, %d material%s"), bSkeletal ? TEXT("a skeletal mesh") : TEXT("a static mesh"),
		bSkeletal ? *FString::Printf(TEXT(" with %d bones"), Bones.Num()) : TEXT(""), NumTriangles(), Materials.Num(), Materials.Num() == 1 ? TEXT("") : TEXT("s"));
}

bool FMantleVehicleChassisSkin::CanBeTwin(const FMantleChassisSpec& Spec)
{
	return Spec.Aircraft.bSet && Spec.Aircraft.bPropeller && Spec.Aircraft.EngineCount == 1;
}

FMantleChassisSpec FMantleVehicleChassisSkin::AsTwin(const FMantleChassisSpec& Spec)
{
	FMantleChassisSpec Twin = Spec;
	if (CanBeTwin(Spec))
	{
		// The thrust a document gives each engine is the handling's: the preset's hand-tuned handling at its own size, derived from the archetype's when resized (which
		// shares it by the engines it has against the archetype's). Both say two engines of half the thrust, so the pair pushes as the one did and the plane flies as tuned.
		Twin.Aircraft.EngineCount = 2;
		Twin.Aircraft.EngineThrustNewtons = Spec.Aircraft.EngineThrustNewtons * 0.5;
		Twin.Archetype.EngineThrustNewtons = Spec.Archetype.EngineThrustNewtons * 0.5;
		Twin.ArchetypeSize.Aircraft.EngineCount = 2;
		Twin.ArchetypeSize.Aircraft.EngineThrustNewtons = Spec.ArchetypeSize.Aircraft.EngineThrustNewtons * 0.5;
	}
	return Twin;
}

int32 FMantleVehicleChassisSkin::CountPropellers(const FMantleVehicleModel& Model)
{
	// The same bones SuggestMap hands to the propeller slots: a propeller rigged as a chain of bones is one propeller.
	return MantleVehicleChassisSkinPrivate::TopBones(Model, [](const FString& Name) { return MantleVehicleChassisSkinPrivate::IsPropellerName(Name); }).Num();
}

TArray<FMantleVehiclePartSlot> FMantleVehicleChassisSkin::PartSlots(const FMantleChassisSpec& Spec, const FMantleSkinnedMesh& Chassis)
{
	using namespace MantleVehicleChassisSkinPrivate;
	TArray<FMantleVehiclePartSlot> Slots;
	const bool bHiddenWheels = FMantleVehicleShape::WheelsHidden(Spec);
	for (int32 Bone = 1; Bone < Chassis.Bones.Num(); ++Bone)
	{
		const FString Name = Chassis.Bones[Bone].Name.ToString();
		FMantleVehiclePartSlot Slot;
		Slot.ChassisBone = Chassis.Bones[Bone].Name;
		if (Name.StartsWith(TEXT("wheel_")))
		{
			// A helicopter's skids and a hovercraft's pads are the chassis's, never drawn, so there is no wheel to assign.
			if (bHiddenWheels)
			{
				continue;
			}
			Slot.Kind = EMantleVehiclePartKind::Wheel;
		}
		else if (Name.StartsWith(TEXT("fork")))
		{
			Slot.Kind = EMantleVehiclePartKind::Fork;
		}
		else if (Name.StartsWith(TEXT("aileron")) || Name == TEXT("elevator") || Name == TEXT("rudder"))
		{
			Slot.Kind = EMantleVehiclePartKind::Surface;
		}
		else if (Name.StartsWith(TEXT("propeller")) || Name == TEXT("main_rotor") || Name == TEXT("tail_rotor"))
		{
			Slot.Kind = EMantleVehiclePartKind::Spinner;
		}
		else
		{
			// The root, a jet's nozzle (it pushes and does not move) and anything else a chassis may add: nothing to assign.
			continue;
		}
		Slot.Label = LabelOf(Name, Spec);
		Slot.Help = HelpOf(Slot.Kind);
		Slots.Add(Slot);
	}
	return Slots;
}

FVector FMantleVehicleChassisSkin::Extent(const FMantleVehicleModel& Model, const double YawDegrees)
{
	FBox Box(ForceInit);
	const FRotator Yaw(0.0, YawDegrees, 0.0);
	for (const FVector3f& Position : Model.LOD.Positions)
	{
		Box += Yaw.RotateVector(FVector(Position));
	}
	return Box.IsValid ? Box.GetSize() : FVector::ZeroVector;
}

FMantleVehicleModelMap FMantleVehicleChassisSkin::SuggestMap(const TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModel& Model, const double YawDegrees)
{
	using namespace MantleVehicleChassisSkinPrivate;
	FMantleVehicleModelMap Map;
	if (!Model.bSkeletal && Model.LOD.Sections.Num() == 0)
	{
		return Map;
	}
	const FRotator Yaw(0.0, YawDegrees, 0.0);
	auto Y = [&](const int32 Bone) { return Yaw.RotateVector(Model.BonePositions[Bone]).Y; };
	auto X = [&](const int32 Bone) { return Yaw.RotateVector(Model.BonePositions[Bone]).X; };

	// The model's bones whose name passes Test, a chain of them counted as its top bone (TopBones).
	auto Bones = [&Model](TFunctionRef<bool(const FString&)> Test) { return TopBones(Model, Test); };
	auto Sections = [&Model](TFunctionRef<bool(const FString&)> Test)
	{
		TArray<int32> Found;
		for (int32 Section = 0; Section < Model.LOD.Sections.Num(); ++Section)
		{
			if (Test(Model.LOD.Sections[Section].MaterialName.ToString().ToLower()))
			{
				Found.Add(Section);
			}
		}
		return Found;
	};
	auto SlotByName = [&Slots](const TCHAR* Name) -> const FMantleVehiclePartSlot*
	{
		return Slots.FindByPredicate([Name](const FMantleVehiclePartSlot& Slot) { return Slot.ChassisBone == FName(Name); });
	};
	auto AssignBone = [&Map, &Model](const FMantleVehiclePartSlot& Slot, const int32 Bone) { Map.Parts.Add(Slot.ChassisBone, FMantleVehicleModelPiece::OfBone(Model.Bones[Bone].Name)); };
	auto AssignSection = [&Map](const FMantleVehiclePartSlot& Slot, const int32 Section) { Map.Parts.Add(Slot.ChassisBone, FMantleVehicleModelPiece::OfSection(Section)); };

	// One or two of a kind: a single slot takes a single candidate; a left and a right slot take a left and a right candidate (by name, else by where they are).
	auto Pair = [&](const FMantleVehiclePartSlot* One, const FMantleVehiclePartSlot* Left, const FMantleVehiclePartSlot* Right, const TArray<int32>& BoneCandidates,
		const TArray<int32>& SectionCandidates)
	{
		if (One && BoneCandidates.Num() == 1)
		{
			AssignBone(*One, BoneCandidates[0]);
		}
		else if (One && BoneCandidates.Num() == 0 && SectionCandidates.Num() == 1)
		{
			AssignSection(*One, SectionCandidates[0]);
		}
		else if (Left && Right && BoneCandidates.Num() == 2)
		{
			int32 L = BoneCandidates[0];
			int32 R = BoneCandidates[1];
			const int32 SideL = SideOf(Model.Bones[L].Name.ToString().ToLower());
			const int32 SideR = SideOf(Model.Bones[R].Name.ToString().ToLower());
			if (SideL > 0 || SideR < 0 || (SideL == 0 && SideR == 0 && Y(L) > Y(R)))
			{
				Swap(L, R);
			}
			AssignBone(*Left, L);
			AssignBone(*Right, R);
		}
	};

	if (Model.bSkeletal)
	{
		Pair(SlotByName(TEXT("propeller")), SlotByName(TEXT("propeller_l")), SlotByName(TEXT("propeller_r")), Bones([](const FString& Name) { return IsPropellerName(Name); }), TArray<int32>());
		Pair(SlotByName(TEXT("main_rotor")), nullptr, nullptr, Bones([](const FString& Name) { return Has(Name, TEXT("rotor")) && !Has(Name, TEXT("tail")) && !Has(Name, TEXT("rear")); }),
			TArray<int32>());
		Pair(SlotByName(TEXT("tail_rotor")), nullptr, nullptr, Bones([](const FString& Name) { return Has(Name, TEXT("rotor")) && (Has(Name, TEXT("tail")) || Has(Name, TEXT("rear"))); }),
			TArray<int32>());
		Pair(SlotByName(TEXT("elevator")), nullptr, nullptr, Bones([](const FString& Name) { return Has(Name, TEXT("elevator")); }), TArray<int32>());
		Pair(SlotByName(TEXT("rudder")), nullptr, nullptr, Bones([](const FString& Name) { return Has(Name, TEXT("rudder")); }), TArray<int32>());
		Pair(nullptr, SlotByName(TEXT("aileron_l")), SlotByName(TEXT("aileron_r")), Bones([](const FString& Name) { return Has(Name, TEXT("aileron")); }), TArray<int32>());

		// A front fork: a bone called a fork, or the front suspension.
		if (const FMantleVehiclePartSlot* Fork = Slots.FindByPredicate([](const FMantleVehiclePartSlot& Slot) { return Slot.Kind == EMantleVehiclePartKind::Fork; }))
		{
			const TArray<int32> Forks = Bones([](const FString& Name) { return Has(Name, TEXT("fork")) || (Has(Name, TEXT("susp")) && (Name.EndsWith(TEXT("_f")) || Has(Name, TEXT("front")))); });
			if (Forks.Num() == 1)
			{
				AssignBone(*Fork, Forks[0]);
			}
		}
	}
	else
	{
		Pair(SlotByName(TEXT("propeller")), nullptr, nullptr, TArray<int32>(), Sections([](const FString& Name) { return Has(Name, TEXT("prop")); }));
		Pair(SlotByName(TEXT("main_rotor")), nullptr, nullptr, TArray<int32>(), Sections([](const FString& Name) { return Has(Name, TEXT("rotor")) && !Has(Name, TEXT("tail")); }));
		Pair(SlotByName(TEXT("tail_rotor")), nullptr, nullptr, TArray<int32>(), Sections([](const FString& Name) { return Has(Name, TEXT("rotor")) && Has(Name, TEXT("tail")); }));
	}

	// The wheels. The candidates are bones with "wheel" in the name; a two-wheeler's rig may call its two "FR" and "RR" (front and rear). Slots are matched only
	// when there are as many candidates as slots, so a model with a spare wheel bone is not guessed at.
	if (Model.bSkeletal)
	{
		TArray<const FMantleVehiclePartSlot*> Centre;
		TArray<const FMantleVehiclePartSlot*> Left;
		TArray<const FMantleVehiclePartSlot*> Right;
		for (const FMantleVehiclePartSlot& Slot : Slots)
		{
			if (Slot.Kind != EMantleVehiclePartKind::Wheel)
			{
				continue;
			}
			const FString Name = Slot.ChassisBone.ToString();
			// wheel_f and wheel_r (front and rear: not "right") and wheel_a1 are on the centre line; wheel_fl, wheel_rr, wheel_a1_l and the like have a side.
			const bool bAxleSided = Name.StartsWith(TEXT("wheel_a")) && (Name.EndsWith(TEXT("_l")) || Name.EndsWith(TEXT("_r")));
			const bool bCornerSided = Name.EndsWith(TEXT("_fl")) || Name.EndsWith(TEXT("_fr")) || Name.EndsWith(TEXT("_rl")) || Name.EndsWith(TEXT("_rr"));
			if (!bAxleSided && !bCornerSided)
			{
				Centre.Add(&Slot);
			}
			else
			{
				(Name.EndsWith(TEXT("l")) ? Left : Right).Add(&Slot);
			}
		}
		const int32 Wanted = Centre.Num() + Left.Num() + Right.Num();
		TArray<int32> Candidates = Bones([](const FString& Name) { return Has(Name, TEXT("wheel")) && !Has(Name, TEXT("steer")) && !Has(Name, TEXT("hub")); });
		if (Candidates.Num() == 0 && Wanted == 2 && Centre.Num() == 2)
		{
			// "FR" and "RR": by their names, which say which is the front.
			const TArray<int32> FrontNamed = Bones([](const FString& Name) { return Name == TEXT("fr") || Name == TEXT("fw") || Name == TEXT("front"); });
			const TArray<int32> RearNamed = Bones([](const FString& Name) { return Name == TEXT("rr") || Name == TEXT("rw") || Name == TEXT("rear"); });
			if (FrontNamed.Num() == 1 && RearNamed.Num() == 1)
			{
				AssignBone(*Centre[0], FrontNamed[0]);
				AssignBone(*Centre[1], RearNamed[0]);
			}
		}
		else if (Candidates.Num() == Wanted && Wanted > 0)
		{
			// The model's own middle, from the candidates themselves; a wheel within a few centimetres of it is on the centre line.
			double MidY = 0.0;
			for (const int32 Bone : Candidates)
			{
				MidY += Y(Bone);
			}
			MidY /= Candidates.Num();
			TArray<int32> OnCentre;
			TArray<int32> OnLeft;
			TArray<int32> OnRight;
			for (const int32 Bone : Candidates)
			{
				const double Across = Y(Bone) - MidY;
				const bool bMiddle = FMath::Abs(Across) < 4.0 || (Left.Num() + Right.Num() == 0);
				(bMiddle ? OnCentre : (Across < 0.0 ? OnLeft : OnRight)).Add(Bone);
			}
			const auto ByFront = [&X](const int32 A, const int32 B) { return X(A) > X(B); };
			OnCentre.Sort(ByFront);
			OnLeft.Sort(ByFront);
			OnRight.Sort(ByFront);
			if (OnCentre.Num() == Centre.Num() && OnLeft.Num() == Left.Num() && OnRight.Num() == Right.Num())
			{
				// The chassis lists its wheels front to rear, so the model's go front to rear too.
				for (int32 Index = 0; Index < Centre.Num(); ++Index)
				{
					AssignBone(*Centre[Index], OnCentre[Index]);
				}
				for (int32 Index = 0; Index < Left.Num(); ++Index)
				{
					AssignBone(*Left[Index], OnLeft[Index]);
				}
				for (int32 Index = 0; Index < Right.Num(); ++Index)
				{
					AssignBone(*Right[Index], OnRight[Index]);
				}
			}
		}
	}
	return Map;
}

double FMantleVehicleChassisSkin::SuggestMapAndYaw(const TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModel& Model, FMantleVehicleModelMap& OutMap)
{
	using namespace MantleVehicleChassisSkinPrivate;
	// Which way is forward: with the model turned by a trial angle, a front wheel (or the main rotor) is ahead of a rear wheel (or the tail rotor), along the length.
	// The turn that puts them so is the answer. Without such a pair the model is left facing the way it was modelled.
	auto IsFront = [](const FString& Name) { return Name == TEXT("wheel_f") || Name == TEXT("wheel_fl") || Name == TEXT("wheel_fr") || Name.StartsWith(TEXT("wheel_a1")); };
	for (const double Turn : { 0.0, 90.0, 180.0, 270.0 })
	{
		FMantleVehicleModelMap Map = SuggestMap(Slots, Model, Turn);
		const FRotator Yaw(0.0, Turn, 0.0);
		FVector Front = FVector::ZeroVector;
		FVector Rear = FVector::ZeroVector;
		int32 Fronts = 0;
		int32 Rears = 0;
		auto Add = [&](const FName ChassisBone, const bool bFront)
		{
			const FMantleVehicleModelPiece* Piece = Map.Find(ChassisBone);
			const int32 Bone = Piece && Piece->Kind == FMantleVehicleModelPiece::EKind::Bone ? Model.FindBone(Piece->Bone) : INDEX_NONE;
			if (Bone != INDEX_NONE)
			{
				(bFront ? Front : Rear) += Yaw.RotateVector(Model.BonePositions[Bone]);
				++(bFront ? Fronts : Rears);
			}
		};
		for (const FMantleVehiclePartSlot& Slot : Slots)
		{
			if (Slot.Kind == EMantleVehiclePartKind::Wheel)
			{
				Add(Slot.ChassisBone, IsFront(Slot.ChassisBone.ToString()));
			}
		}
		if (Fronts == 0 || Rears == 0)
		{
			Fronts = 0;
			Rears = 0;
			Front = FVector::ZeroVector;
			Rear = FVector::ZeroVector;
			Add(FName(TEXT("main_rotor")), true);
			Add(FName(TEXT("tail_rotor")), false);
		}
		if (Fronts == 0 || Rears == 0)
		{
			continue;
		}
		const FVector Ahead = Front / Fronts - Rear / Rears;
		if (Ahead.X > 0.0 && Ahead.X > 0.7 * Ahead.Size2D())
		{
			OutMap = Map;
			return Turn;
		}
	}
	OutMap = SuggestMap(Slots, Model, 0.0);
	return 0.0;
}

FMantleVehicleModelProposal FMantleVehicleChassisSkin::Propose(const FMantleChassisSpec& Archetype, const FMantleVehiclePreset& Preset, const FMantleVehicleModel& Model,
	const double YawDegrees, const FMantleVehicleModelMap& Map)
{
	using namespace MantleVehicleChassisSkinPrivate;
	FMantleVehicleModelProposal Out;
	const FVector Size = Extent(Model, YawDegrees);
	Out.bUsesSize = FMantleVehicleShape::UsesSize(Archetype);
	const FString Kind = Preset.Name.IsEmpty() ? Preset.Id : Preset.Name;

	if (Out.bUsesSize)
	{
		FMantleVehicleAsset Grey;
		FString Why;
		if (!FMantleVehicleChassisBuilder::MakeAsset(Archetype, Grey, Why) || Grey.Body.LODs.Num() == 0)
		{
			Out.Notes.Add(FString::Printf(TEXT("Mantle could not size the %s to your model, so it is at its standard size."), *Kind));
			return Out;
		}
		const FBox3f Box = Grey.Body.ComputeBounds(0);
		double ModelLength = Size.X;
		double ModelWidth = Size.Y;
		double GreyLength = Box.Max.X - Box.Min.X;
		double GreyWidth = Box.Max.Y - Box.Min.Y;
		if (Archetype.Helicopter.bSet)
		{
			// A helicopter's footprint is its rotor's: what a creator's model shows at rest depends on where its blades lie, so it is sized by its longest side.
			ModelLength = ModelWidth = FMath::Max(Size.X, Size.Y);
			GreyLength = GreyWidth = FMath::Max(GreyLength, GreyWidth);
		}
		if (Archetype.Aircraft.bSet)
		{
			// A plane and a jet are tuned to fly level at their standard size (the flight probe: a jet at Size 1 holds its height within 2 m over 20 s and at 0.95 or 1.05 within
			// 40; its trim is not worked out again for other sizes), so the chassis stays at it and the model is scaled to fit.
			Out.Size = 1.0;
			if (!(ModelLength > 1.0 && ModelWidth > 1.0))
			{
				Out.Notes.Add(TooSmallNote());
				return Out;
			}
			const double Ratio = FMath::Sqrt((ModelLength * ModelWidth) / (GreyLength * GreyWidth));
			// The percentage quoted is the one the model is really placed at, which the scale limits may hold short of what its size asks for.
			FString HeldNote;
			const double Placed = HeldModelScale(1.0 / Ratio, HeldNote);
			Out.Notes.Add(FMath::Abs(Ratio - 1.0) < 0.03 && HeldNote.IsEmpty() ? FString::Printf(TEXT("Your model is the size of a standard %s."), *Kind)
				: FString::Printf(TEXT("A %s is tuned to fly steadily at its standard size, so it stays at it and your model is %s to %.0f%% of its size to fit (change the Size to make the %s bigger or smaller, which can make it drift in the air)."),
					*Kind, Placed < 1.0 ? TEXT("shrunk") : TEXT("enlarged"), 100.0 * Placed, *Kind));
			if (!HeldNote.IsEmpty())
			{
				Out.Notes.Add(HeldNote);
			}
			return Out;
		}
		const FMantleVehicleFit Fit = FMantleVehicleFitter::ComputeFromFootprint(Preset, ModelLength, ModelWidth, GreyLength, GreyWidth);
		Out.Size = Fit.Scale;
		Out.bClamped = Fit.bClamped;
		Out.Notes.Add(Fit.Reason);
		return Out;
	}

	// A road vehicle: the body from the model's bounds; the wheels from the parts assigned to them, else the preset's proportions at the model's length.
	const FMantleVehicleDimensions Standard = FMantleVehicleShape::ArchetypeDimensionsOf(Archetype);
	if (Standard.BodyLength < 1.0 || Size.X < 1.0)
	{
		if (Size.X < 1.0)
		{
			Out.Notes.Add(TooSmallNote());
		}
		return Out;
	}
	const double Factor = FMath::Clamp(Size.X / Standard.BodyLength, 0.3, 4.0);
	TMap<EMantleVehicleDimension, double> Wanted;
	for (const EMantleVehicleDimension Dimension : MantleVehicleDimensions)
	{
		Wanted.Add(Dimension, Standard.Get(Dimension) * Factor);
	}
	Wanted[EMantleVehicleDimension::BodyLength] = Size.X;
	Wanted[EMantleVehicleDimension::BodyWidth] = FMath::Max(Size.Y * 0.9, 1.0);

	// The wheels the creator assigned (a bone or a section of the model): their size and where they are say the wheelbase and the wheel.
	TArray<int32> Heaviest = HeaviestBones(Model);
	FBox FrontBox(ForceInit);
	FBox RearBox(ForceInit);
	FBox LeftBox(ForceInit);
	FBox RightBox(ForceInit);
	FMantleVehicleModelPlacement Turn;
	Turn.YawDegrees = YawDegrees;
	double RadiusSum = 0.0;
	double WidthSum = 0.0;
	int32 Counted = 0;
	TArray<int32> Vertices;
	for (const TPair<FName, FMantleVehicleModelPiece>& Part : Map.Parts)
	{
		const FString Name = Part.Key.ToString();
		if (!Name.StartsWith(TEXT("wheel_")))
		{
			continue;
		}
		PieceVertices(Model, Heaviest, Part.Value, Vertices);
		const FBox Box = PointsBox(Model, Vertices, Turn);
		if (!Box.IsValid || Box.GetSize().Y < 0.5)
		{
			continue;
		}
		const bool bFront = IsFrontWheelName(Name);
		(bFront ? FrontBox : RearBox) += Box;
		if (Name.EndsWith(TEXT("l")))
		{
			LeftBox += Box;
		}
		else if (Name.EndsWith(TEXT("r")) && Name != TEXT("wheel_r"))
		{
			RightBox += Box;
		}
		RadiusSum += 0.5 * FMath::Max(Box.GetSize().X, Box.GetSize().Z);
		WidthSum += Box.GetSize().Y;
		++Counted;
	}
	if (Counted >= 2 && FrontBox.IsValid && RearBox.IsValid)
	{
		const double Radius = RadiusSum / Counted;
		Wanted[EMantleVehicleDimension::WheelRadius] = Radius;
		Wanted[EMantleVehicleDimension::WheelWidth] = FMath::Max(WidthSum / Counted, 1.0);
		const double Wheelbase = FrontBox.GetCenter().X - RearBox.GetCenter().X;
		if (Wheelbase > 1.0)
		{
			Wanted[EMantleVehicleDimension::Wheelbase] = Wheelbase;
		}
		if (LeftBox.IsValid && RightBox.IsValid)
		{
			Wanted[EMantleVehicleDimension::Track] = FMath::Max(RightBox.GetCenter().Y - LeftBox.GetCenter().Y, 1.0);
		}
		Wanted[EMantleVehicleDimension::GroundClearance] = FMath::Max(Standard.GroundClearance * Factor, Radius * 0.5);
		Out.Notes.Add(FString::Printf(TEXT("The chassis's wheels are placed where your model's wheels are (a %.0f cm wheelbase)."), Wheelbase));
	}
	else
	{
		Out.Notes.Add(FString::Printf(TEXT("The %s is sized to your model's length (%.0f cm) with a standard %s's proportions. Assign its wheels below to place the chassis's wheels exactly where yours are."),
			*Kind, Size.X, *Kind));
	}
	// Wheels first (they bound how short the wheelbase may be), then the rest.
	for (const EMantleVehicleDimension Dimension : { EMantleVehicleDimension::WheelRadius, EMantleVehicleDimension::WheelWidth, EMantleVehicleDimension::Wheelbase,
			 EMantleVehicleDimension::Track, EMantleVehicleDimension::BodyLength, EMantleVehicleDimension::BodyWidth, EMantleVehicleDimension::BodyHeight,
			 EMantleVehicleDimension::GroundClearance })
	{
		if (FMantleVehicleShape::IsEditable(Archetype, Dimension))
		{
			Out.Dimensions.Add(TPair<EMantleVehicleDimension, double>(Dimension, Wanted[Dimension]));
		}
	}
	return Out;
}

FMantleVehicleModelPlacement FMantleVehicleChassisSkin::FitPlacement(const FMantleChassisSpec& Spec, const FMantleSkinnedMesh& Chassis, const FMantleVehicleModel& Model,
	const double YawDegrees, const FMantleVehicleModelMap& Map)
{
	using namespace MantleVehicleChassisSkinPrivate;
	FMantleVehicleModelPlacement Placement;
	Placement.YawDegrees = YawDegrees;
	const FVector Size = Extent(Model, YawDegrees);
	if (Chassis.LODs.Num() == 0 || Size.X < 1.0 || Size.Y < 1.0)
	{
		if (Chassis.LODs.Num() > 0)
		{
			Placement.Note = TooSmallNote();
		}
		return Placement;
	}
	const FBox3f GreyBox = Chassis.ComputeBounds(0);
	const double GreyLength = GreyBox.Max.X - GreyBox.Min.X;
	const double GreyWidth = GreyBox.Max.Y - GreyBox.Min.Y;
	const FVector GreyCentre(0.5 * (GreyBox.Min.X + GreyBox.Max.X), 0.5 * (GreyBox.Min.Y + GreyBox.Max.Y), 0.0);

	// The scale: a vehicle shaped by one Size covers the same ground as the model; a road vehicle is as long as it.
	if (Spec.Helicopter.bSet)
	{
		Placement.Scale = FMath::Max(GreyLength, GreyWidth) / FMath::Max(Size.X, Size.Y);
	}
	else if (FMantleVehicleShape::UsesSize(Spec))
	{
		Placement.Scale = FMath::Sqrt((GreyLength * GreyWidth) / (Size.X * Size.Y));
	}
	else
	{
		Placement.Scale = Spec.Length > 1.0 ? Spec.Length / Size.X : 1.0;
	}
	Placement.Scale = HeldModelScale(Placement.Scale, Placement.Note);

	// Where it stands: the model's own box centred on the chassis's (wheels, when two or more are assigned, decide the length instead), its lowest point, or its
	// wheels' lowest, on the ground.
	FMantleVehicleModelPlacement Turned = Placement;
	FBox Moved(ForceInit);
	for (const FVector3f& Position : Model.LOD.Positions)
	{
		Moved += Turned.Point(FVector(Position));
	}
	FVector Centre = Moved.GetCenter();
	double Floor = Moved.Min.Z;

	const TArray<int32> Heaviest = HeaviestBones(Model);
	FBox FrontWheels(ForceInit);
	FBox RearWheels(ForceInit);
	double WheelFloor = TNumericLimits<double>::Max();
	TArray<int32> Vertices;
	for (const TPair<FName, FMantleVehicleModelPiece>& Part : Map.Parts)
	{
		const FString Name = Part.Key.ToString();
		if (!Name.StartsWith(TEXT("wheel_")) || FMantleVehicleShape::WheelsHidden(Spec))
		{
			continue;
		}
		PieceVertices(Model, Heaviest, Part.Value, Vertices);
		const FBox Box = PointsBox(Model, Vertices, Turned);
		if (!Box.IsValid)
		{
			continue;
		}
		const bool bFront = IsFrontWheelName(Name);
		(bFront ? FrontWheels : RearWheels) += Box;
		WheelFloor = FMath::Min(WheelFloor, Box.Min.Z);
	}
	if (FrontWheels.IsValid && RearWheels.IsValid && !FMantleVehicleShape::UsesSize(Spec))
	{
		// A road vehicle with its wheels assigned: the middle of the wheelbase is the chassis's origin.
		Centre.X = 0.5 * (FrontWheels.GetCenter().X + RearWheels.GetCenter().X);
		Floor = WheelFloor;
	}
	else if (WheelFloor < TNumericLimits<double>::Max() && FMantleVehicleShape::UsesSize(Spec))
	{
		// A plane with its wheels assigned stands on them.
		Floor = WheelFloor;
	}
	Placement.Offset = FVector(GreyCentre.X - Centre.X, GreyCentre.Y - Centre.Y, -Floor);
	if (FrontWheels.IsValid && RearWheels.IsValid && !FMantleVehicleShape::UsesSize(Spec))
	{
		Placement.Offset.X = -Centre.X;
	}
	return Placement;
}

bool FMantleVehicleChassisSkin::Skin(const FMantleSkinnedMesh& Chassis, const TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModel& Model,
	const FMantleVehicleModelPlacement& Placement, const FMantleVehicleModelMap& Map, FMantleVehicleSkinned& Out, FString& OutError, const double MaxWheelMove)
{
	using namespace MantleVehicleChassisSkinPrivate;
	Out = FMantleVehicleSkinned();
	Out.Placement = Placement;
	const FMantleSkinnedMeshLOD& Source = Model.LOD;
	const int32 NumVertices = Source.Positions.Num();
	if (NumVertices == 0 || Source.Indices.Num() < 3 || Chassis.Bones.Num() == 0 || Chassis.ReferencePose.Num() != Chassis.Bones.Num())
	{
		OutError = TEXT("there is no model geometry to put on the chassis");
		return false;
	}

	FMantleSkinnedMesh& Body = Out.Body;
	Body.Bones = Chassis.Bones;
	Body.ReferencePose = Chassis.ReferencePose;
	FMantleSkinnedMeshLOD& LOD = Body.LODs.AddDefaulted_GetRef();
	LOD.NumInfluences = 1;
	LOD.bFullPrecisionUVs = true;
	LOD.Positions.SetNumUninitialized(NumVertices);
	LOD.Normals.SetNumUninitialized(NumVertices);
	LOD.UVs = Source.UVs;
	LOD.Indices = Source.Indices;
	// The model's other UV channels and vertex colours ride along, one entry per vertex like the rest (a copied vertex below copies them too).
	for (const TArray<FVector2f>& Channel : Model.ExtraUVs)
	{
		if (Channel.Num() == NumVertices)
		{
			Out.Extras.UVs.Add(Channel);
		}
	}
	if (Model.Colors.Num() == NumVertices)
	{
		Out.Extras.Colors = Model.Colors;
	}
	const bool bTangents = Source.Tangents.Num() == NumVertices;
	if (bTangents)
	{
		LOD.Tangents.SetNumUninitialized(NumVertices);
	}
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		LOD.Positions[Vertex] = FVector3f(Placement.Point(FVector(Source.Positions[Vertex])));
		LOD.Normals[Vertex] = FVector3f(Placement.Direction(FVector(Source.Normals[Vertex])).GetSafeNormal());
		if (bTangents)
		{
			const FVector Tangent = Placement.Direction(FVector(Source.Tangents[Vertex].X, Source.Tangents[Vertex].Y, Source.Tangents[Vertex].Z));
			LOD.Tangents[Vertex] = FVector4f(FVector3f(Tangent), Source.Tangents[Vertex].W);
		}
	}

	// Which chassis bone each vertex rides: the root, unless its part is assigned to a moving bone.
	TArray<int32> BoneOf;
	BoneOf.Init(0, NumVertices);
	TMap<FName, int32> ChassisIndex;
	for (int32 Bone = 0; Bone < Body.Bones.Num(); ++Bone)
	{
		ChassisIndex.Add(Body.Bones[Bone].Name, Bone);
	}

	// A model bone's part: the nearest assigned bone at or above it (a wheel assigned inside an assigned fork stays the wheel's).
	if (Model.bSkeletal)
	{
		TArray<int32> Target;
		Target.Init(INDEX_NONE, Model.Bones.Num());
		for (int32 Bone = 0; Bone < Model.Bones.Num(); ++Bone)
		{
			const int32 Parent = Model.Bones[Bone].ParentIndex;
			Target[Bone] = Parent >= 0 && Parent < Bone ? Target[Parent] : INDEX_NONE;
			for (const TPair<FName, FMantleVehicleModelPiece>& Part : Map.Parts)
			{
				if (Part.Value.Kind == FMantleVehicleModelPiece::EKind::Bone && Part.Value.Bone == Model.Bones[Bone].Name && ChassisIndex.Contains(Part.Key))
				{
					Target[Bone] = ChassisIndex[Part.Key];
				}
			}
		}
		const TArray<int32> Heaviest = HeaviestBones(Model);
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			if (Heaviest[Vertex] != INDEX_NONE && Target.IsValidIndex(Heaviest[Vertex]) && Target[Heaviest[Vertex]] != INDEX_NONE)
			{
				BoneOf[Vertex] = Target[Heaviest[Vertex]];
			}
		}
	}

	// A section's part: a vertex shared with another section is copied, so the part takes its own and the rest of the model keeps the original.
	TArray<int32> FirstSection;
	TBitArray<> Shared(false, NumVertices);
	FirstSection.Init(INDEX_NONE, NumVertices);
	for (int32 Section = 0; Section < Source.Sections.Num(); ++Section)
	{
		const FMantleSkinnedMeshSection& S = Source.Sections[Section];
		for (int32 At = S.FirstIndex; At < S.FirstIndex + S.IndexCount && At < LOD.Indices.Num(); ++At)
		{
			const int32 Vertex = static_cast<int32>(LOD.Indices[At]);
			if (FirstSection[Vertex] == INDEX_NONE)
			{
				FirstSection[Vertex] = Section;
			}
			else if (FirstSection[Vertex] != Section)
			{
				Shared[Vertex] = true;
			}
		}
	}
	for (const TPair<FName, FMantleVehicleModelPiece>& Part : Map.Parts)
	{
		const int32* PartBone = ChassisIndex.Find(Part.Key);
		if (!PartBone || Part.Value.Kind != FMantleVehicleModelPiece::EKind::Section || !Source.Sections.IsValidIndex(Part.Value.Section))
		{
			continue;
		}
		const FMantleSkinnedMeshSection& S = Source.Sections[Part.Value.Section];
		TMap<int32, int32> Copy;
		for (int32 At = S.FirstIndex; At < S.FirstIndex + S.IndexCount && At < LOD.Indices.Num(); ++At)
		{
			const int32 Vertex = static_cast<int32>(LOD.Indices[At]);
			if (!Shared[Vertex])
			{
				BoneOf[Vertex] = *PartBone;
				continue;
			}
			int32* Made = Copy.Find(Vertex);
			if (!Made)
			{
				// Copies first: an element of an array may not be added to the same array.
				const int32 Added = LOD.Positions.Num();
				const FVector3f CopiedPosition = LOD.Positions[Vertex];
				const FVector3f CopiedNormal = LOD.Normals[Vertex];
				const FVector2f CopiedUV = LOD.UVs[Vertex];
				LOD.Positions.Add(CopiedPosition);
				LOD.Normals.Add(CopiedNormal);
				LOD.UVs.Add(CopiedUV);
				for (TArray<FVector2f>& ExtraChannel : Out.Extras.UVs)
				{
					const FVector2f CopiedExtra = ExtraChannel[Vertex];
					ExtraChannel.Add(CopiedExtra);
				}
				if (Out.Extras.Colors.Num() > 0)
				{
					const FColor CopiedColor = Out.Extras.Colors[Vertex];
					Out.Extras.Colors.Add(CopiedColor);
				}
				if (bTangents)
				{
					const FVector4f CopiedTangent = LOD.Tangents[Vertex];
					LOD.Tangents.Add(CopiedTangent);
				}
				BoneOf.Add(*PartBone);
				Made = &Copy.Add(Vertex, Added);
			}
			LOD.Indices[At] = static_cast<uint32>(*Made);
		}
	}

	// Each moving bone's part: its vertices and, for a bone that turns about a hub or a hinge, where that is.
	const FMantleVehicleModelPlacement Turn = Placement;
	for (const FMantleVehiclePartSlot& Slot : Slots)
	{
		const FMantleVehicleModelPiece* Piece = Map.Find(Slot.ChassisBone);
		const int32* BoneIndex = ChassisIndex.Find(Slot.ChassisBone);
		if (!Piece || !Piece->IsSet() || !BoneIndex)
		{
			continue;
		}
		FBox Box(ForceInit);
		int32 Count = 0;
		for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
		{
			if (BoneOf[Vertex] == *BoneIndex)
			{
				Box += FVector(LOD.Positions[Vertex]);
				++Count;
			}
		}
		if (Count == 0)
		{
			Out.Notes.Add(FString::Printf(TEXT("Nothing in your model is weighted to the part you picked for the %s, so it will not move."), *Slot.Label.ToString()));
			continue;
		}
		// A model bone's own position is the best pivot; a section has only its middle.
		FVector Pivot = Box.GetCenter();
		if (Piece->Kind == FMantleVehicleModelPiece::EKind::Bone)
		{
			const int32 ModelBone = Model.FindBone(Piece->Bone);
			if (ModelBone != INDEX_NONE)
			{
				Pivot = Turn.Point(Model.BonePositions[ModelBone]);
			}
		}
		if (Slot.Kind == EMantleVehiclePartKind::Spinner || Slot.Kind == EMantleVehiclePartKind::Surface)
		{
			// The bones of these parts are the root's children, so the local pose is the model-space one.
			if (Body.Bones[*BoneIndex].ParentIndex == 0)
			{
				FTransform Pose = Body.ReferencePose[*BoneIndex];
				Pose.SetTranslation(Pivot);
				Body.ReferencePose[*BoneIndex] = Pose;
			}
		}
		else if (Slot.Kind == EMantleVehiclePartKind::Wheel)
		{
			// Where the chassis's wheel is: its bone's place in the chassis (the fork it hangs under adds its own).
			FVector Centre = FVector::ZeroVector;
			for (int32 Bone = *BoneIndex; Bone != INDEX_NONE; Bone = Body.Bones[Bone].ParentIndex)
			{
				Centre += Chassis.ReferencePose[Bone].GetLocation();
			}
			const FVector Move = Centre - Pivot;
			if (Move.Size() > MaxWheelMove)
			{
				// Too far to move without tearing the wheel from its strut, and turning it about the chassis's wheel would swing it round that point: it stays still.
				for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
				{
					BoneOf[Vertex] = BoneOf[Vertex] == *BoneIndex ? 0 : BoneOf[Vertex];
				}
				Out.Notes.Add(FString::Printf(TEXT("Your model's %s is %.0f cm from where the chassis's wheel is, so it stays still instead of turning about the wrong point."), *Slot.Label.ToString().ToLower(), Move.Size()));
				continue;
			}
			if (Move.Size() > 2.0)
			{
				Out.Notes.Add(FString::Printf(TEXT("Your model's %s was moved %.0f cm to sit where the chassis's wheel is, so it turns about the right point."), *Slot.Label.ToString().ToLower(), Move.Size()));
			}
			for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
			{
				if (BoneOf[Vertex] == *BoneIndex)
				{
					LOD.Positions[Vertex] += FVector3f(Move);
				}
			}
		}
	}

	// One bone and all its weight per vertex.
	LOD.InfluenceBones.SetNumUninitialized(LOD.Positions.Num());
	LOD.InfluenceWeights.SetNumUninitialized(LOD.Positions.Num());
	for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
	{
		LOD.InfluenceBones[Vertex] = static_cast<uint16>(BoneOf[Vertex]);
		LOD.InfluenceWeights[Vertex] = 255;
	}

	// The sections and their material slots, as the model has them; slot names must be unique and real.
	TSet<FName> Taken;
	TArray<FName> SlotNames;
	for (int32 Slot = 0; Slot < Model.Materials.Num(); ++Slot)
	{
		FName Name = Model.Materials[Slot].MaterialSlotName;
		if (Name.IsNone() || Taken.Contains(Name))
		{
			Name = FName(*FString::Printf(TEXT("%s_%d"), Name.IsNone() ? TEXT("material") : *Name.ToString(), Slot));
		}
		Taken.Add(Name);
		SlotNames.Add(Name);
		FSkeletalMaterial Material = Model.Materials[Slot];
		Material.MaterialSlotName = Name;
		Material.ImportedMaterialSlotName = Name;
		Out.Materials.Add(Material);
	}
	if (Out.Materials.Num() == 0)
	{
		Out.Materials.Add(FSkeletalMaterial(nullptr, FName(TEXT("model"))));
		SlotNames.Add(FName(TEXT("model")));
	}
	for (const FMantleSkinnedMeshSection& Section : Source.Sections)
	{
		FMantleSkinnedMeshSection& Added = LOD.Sections.Add_GetRef(Section);
		Added.MaterialIndex = SlotNames.IsValidIndex(Section.MaterialIndex) ? Section.MaterialIndex : 0;
		Added.MaterialName = SlotNames[Added.MaterialIndex];
	}
	if (LOD.Sections.Num() == 0)
	{
		FMantleSkinnedMeshSection& Whole = LOD.Sections.AddDefaulted_GetRef();
		Whole.MaterialIndex = 0;
		Whole.FirstIndex = 0;
		Whole.IndexCount = LOD.Indices.Num();
		Whole.MaterialName = SlotNames[0];
	}

	FString Why;
	if (!Body.Validate(Why))
	{
		OutError = FString::Printf(TEXT("the model does not make a valid body (%s)"), *Why);
		return false;
	}
	return true;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisSkinPrivate
{
	/** The box around a box of the model's, once the model is turned, scaled and moved onto the chassis. */
	FBox PlaceBox(const FBox& Box, const FMantleVehicleModelPlacement& Placement)
	{
		FBox Out(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Out += Placement.Point(FVector((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z));
		}
		return Out;
	}

	/** A bone named like something that spins or rolls: its collision shape is not the body's. */
	bool LooksLikeMovingPartName(const FString& Lower)
	{
		return Has(Lower, TEXT("wheel")) || Has(Lower, TEXT("tire")) || Has(Lower, TEXT("tyre")) || Has(Lower, TEXT("prop")) || Has(Lower, TEXT("rotor")) || Has(Lower, TEXT("blade"));
	}

	/** The model's bones that are, or hang under, a bone assigned to a wheel, a fork, a propeller or a rotor, or that are named like one. */
	TBitArray<> MovingModelBones(const FMantleVehicleModel& Model, TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModelMap& Map)
	{
		TBitArray<> Moving(false, Model.Bones.Num());
		for (int32 Bone = 0; Bone < Model.Bones.Num(); ++Bone)
		{
			bool bMoves = LooksLikeMovingPartName(Model.Bones[Bone].Name.ToString().ToLower());
			for (const FMantleVehiclePartSlot& Slot : Slots)
			{
				const FMantleVehicleModelPiece* Piece = Slot.Kind == EMantleVehiclePartKind::Surface ? nullptr : Map.Find(Slot.ChassisBone);
				if (Piece && Piece->Kind == FMantleVehicleModelPiece::EKind::Bone)
				{
					const int32 Assigned = Model.FindBone(Piece->Bone);
					bMoves = bMoves || (Assigned != INDEX_NONE && IsUnder(Model, Bone, Assigned));
				}
			}
			Moving[Bone] = bMoves;
		}
		return Moving;
	}

	/**
	 * Boxes as the document holds them: inside +-1000 cm, no lower than Floor (the chassis's own box: a gear or a wheel the model draws below the body must not drag on the
	 * ground), at least 2 cm across, the one the seat sits in first (its exits are beside it) and the rest largest first. Returns how many it wrote to Out.
	 */
	int32 ToChassisBoxes(const TArray<FBox>& Boxes, const double Floor, const FVector& Seat, TArray<FMantleChassisBox>& Out)
	{
		constexpr double Limit = 1000.0;
		Out.Reset();
		for (const FBox& Box : Boxes)
		{
			if (Box.IsValid == 0)
			{
				continue;
			}
			FVector Min = Box.Min.ComponentMax(FVector(-Limit));
			FVector Max = Box.Max.ComponentMin(FVector(Limit));
			Min.Z = FMath::Max(Min.Z, Floor);
			if (Max.X < Min.X || Max.Y < Min.Y || Max.Z < Min.Z + 1.0)
			{
				continue;
			}
			FMantleChassisBox& Added = Out.AddDefaulted_GetRef();
			Added.Center = 0.5 * (Min + Max);
			Added.Extent = (0.5 * (Max - Min)).ComponentMax(FVector(1.0));
		}
		Out.Sort([](const FMantleChassisBox& A, const FMantleChassisBox& B) { return A.Extent.X * A.Extent.Y * A.Extent.Z > B.Extent.X * B.Extent.Y * B.Extent.Z; });
		// The box the seat sits in leads: the exits are guessed beside the first box, and a wing is not where a pilot gets out.
		const int32 Seated = Out.IndexOfByPredicate([&Seat](const FMantleChassisBox& Box) { const FVector Off = (Seat - Box.Center).GetAbs(); const FVector Reach = Box.Extent + FVector(2.0); return Off.X <= Reach.X && Off.Y <= Reach.Y && Off.Z <= Reach.Z; });
		if (Seated > 0)
		{
			const FMantleChassisBox Lead = Out[Seated];
			Out.RemoveAt(Seated);
			Out.Insert(Lead, 0);
		}
		return Out.Num();
	}

	FString VehicleCountText(const int32 Count, const TCHAR* One, const TCHAR* Many)
	{
		return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? One : Many);
	}
}

TArray<FBox> FMantleVehicleChassisSkin::ReduceBoxes(TArray<FBox> Boxes, const int32 MaxBoxes)
{
	Boxes.RemoveAll([](const FBox& Box) { return Box.IsValid == 0; });
	// A physics asset with hundreds of shapes: the smallest are dropped first (they hold little of the shape), so the pairwise merge below stays quick.
	constexpr int32 MaxMerged = 64;
	if (Boxes.Num() > MaxMerged)
	{
		Boxes.Sort([](const FBox& A, const FBox& B) { return A.GetVolume() > B.GetVolume(); });
		Boxes.SetNum(MaxMerged);
	}
	while (Boxes.Num() > FMath::Max(MaxBoxes, 1))
	{
		int32 BestA = 0;
		int32 BestB = 1;
		double BestAdded = TNumericLimits<double>::Max();
		for (int32 A = 0; A < Boxes.Num(); ++A)
		{
			for (int32 B = A + 1; B < Boxes.Num(); ++B)
			{
				const double Added = (Boxes[A] + Boxes[B]).GetVolume() - Boxes[A].GetVolume() - Boxes[B].GetVolume();
				if (Added < BestAdded)
				{
					BestAdded = Added;
					BestA = A;
					BestB = B;
				}
			}
		}
		const FBox Merged = Boxes[BestA] + Boxes[BestB];
		Boxes.RemoveAt(BestB);
		Boxes[BestA] = Merged;
	}
	return Boxes;
}

TArray<FBox> FMantleVehicleChassisSkin::BoxesAroundSurface(const TConstArrayView<FVector3f> Positions, const TConstArrayView<uint32> Indices, const int32 MaxBoxes)
{
	TArray<FBox> Result;

	// The triangles whose corners exist, as points three to a triangle.
	TArray<FVector> Corners;
	Corners.Reserve(Indices.Num());
	for (int32 At = 0; At + 2 < Indices.Num(); At += 3)
	{
		const int32 I0 = static_cast<int32>(Indices[At]);
		const int32 I1 = static_cast<int32>(Indices[At + 1]);
		const int32 I2 = static_cast<int32>(Indices[At + 2]);
		if (!Positions.IsValidIndex(I0) || !Positions.IsValidIndex(I1) || !Positions.IsValidIndex(I2))
		{
			continue;
		}
		Corners.Add(FVector(Positions[I0]));
		Corners.Add(FVector(Positions[I1]));
		Corners.Add(FVector(Positions[I2]));
	}
	if (Corners.IsEmpty())
	{
		// FBox's constructor from a point list reads the first point, so an empty list is refused before it.
		return Result;
	}
	const FBox Bounds(Corners);
	if (Bounds.IsValid == 0)
	{
		return Result;
	}
	if (MaxBoxes <= 1)
	{
		Result.Add(Bounds);
		return Result;
	}

	// A coarse grid over the box; each cell keeps the box around the surface samples that fall in it.
	constexpr int32 GridMax = 24;
	const FVector Size = Bounds.GetSize();
	int32 Dim[3];
	double Cell[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Dim[Axis] = FMath::Clamp(FMath::CeilToInt(Size[Axis]), 1, GridMax);
		Cell[Axis] = FMath::Max(Size[Axis], 0.01) / Dim[Axis];
	}
	TArray<FBox> CellBox;
	CellBox.Init(FBox(ForceInit), Dim[0] * Dim[1] * Dim[2]);
	const auto IndexOf = [&Dim](const int32 X, const int32 Y, const int32 Z) { return (Z * Dim[1] + Y) * Dim[0] + X; };
	const auto Sample = [&](const FVector& Point)
	{
		int32 At[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			At[Axis] = FMath::Clamp(static_cast<int32>((Point[Axis] - Bounds.Min[Axis]) / Cell[Axis]), 0, Dim[Axis] - 1);
		}
		CellBox[IndexOf(At[0], At[1], At[2])] += Point;
	};
	// Each triangle is sampled on a lattice finer than a cell (its corners included), so a thin wing or a tall fin lands in every cell it crosses.
	const double Step = FMath::Max(0.5 * FMath::Min3(Cell[0], Cell[1], Cell[2]), 0.01);
	const int32 MaxLattice = Corners.Num() / 3 > 50000 ? 12 : 32;
	for (int32 At = 0; At + 2 < Corners.Num(); At += 3)
	{
		const FVector& A = Corners[At];
		const FVector& B = Corners[At + 1];
		const FVector& C = Corners[At + 2];
		const double Longest = FMath::Max3((B - A).Size(), (C - B).Size(), (A - C).Size());
		const int32 K = FMath::Clamp(FMath::CeilToInt(Longest / Step), 1, MaxLattice);
		for (int32 I = 0; I <= K; ++I)
		{
			for (int32 J = 0; J <= K - I; ++J)
			{
				Sample(A + (B - A) * (static_cast<double>(I) / K) + (C - A) * (static_cast<double>(J) / K));
			}
		}
	}

	// A region is a block of cells, tight to the cells that hold surface.
	struct FRegion
	{
		int32 Lo[3] = { 0, 0, 0 };
		int32 Hi[3] = { 0, 0, 0 };
		int32 Occupied = 0;
		bool bSplittable = true;

		int64 Cells() const { return static_cast<int64>(Hi[0] - Lo[0] + 1) * (Hi[1] - Lo[1] + 1) * (Hi[2] - Lo[2] + 1); }
	};
	const auto Tighten = [&](FRegion& Region) -> bool
	{
		int32 Lo[3] = { MAX_int32, MAX_int32, MAX_int32 };
		int32 Hi[3] = { -1, -1, -1 };
		int32 Occupied = 0;
		for (int32 Z = Region.Lo[2]; Z <= Region.Hi[2]; ++Z)
		{
			for (int32 Y = Region.Lo[1]; Y <= Region.Hi[1]; ++Y)
			{
				for (int32 X = Region.Lo[0]; X <= Region.Hi[0]; ++X)
				{
					if (CellBox[IndexOf(X, Y, Z)].IsValid != 0)
					{
						++Occupied;
						Lo[0] = FMath::Min(Lo[0], X);
						Lo[1] = FMath::Min(Lo[1], Y);
						Lo[2] = FMath::Min(Lo[2], Z);
						Hi[0] = FMath::Max(Hi[0], X);
						Hi[1] = FMath::Max(Hi[1], Y);
						Hi[2] = FMath::Max(Hi[2], Z);
					}
				}
			}
		}
		if (Occupied == 0)
		{
			return false;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Region.Lo[Axis] = Lo[Axis];
			Region.Hi[Axis] = Hi[Axis];
		}
		Region.Occupied = Occupied;
		return true;
	};

	TArray<FRegion> Regions;
	{
		FRegion Whole;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Whole.Hi[Axis] = Dim[Axis] - 1;
		}
		if (!Tighten(Whole))
		{
			Result.Add(Bounds);
			return Result;
		}
		Regions.Add(Whole);
	}
	// Cut the region with the most empty cells where the two halves leave the least empty space around the parts, until the boxes are used up or none is worth cutting.
	while (Regions.Num() < MaxBoxes)
	{
		int32 Pick = INDEX_NONE;
		int64 PickWaste = 0;
		for (int32 Index = 0; Index < Regions.Num(); ++Index)
		{
			const int64 Waste = Regions[Index].Cells() - Regions[Index].Occupied;
			if (Regions[Index].bSplittable && Waste > PickWaste)
			{
				Pick = Index;
				PickWaste = Waste;
			}
		}
		if (Pick == INDEX_NONE)
		{
			break;
		}
		const int64 Cells = Regions[Pick].Cells();
		if (PickWaste * 10 < Cells * 3)
		{
			// At least 70 per cent full: a box is a good fit already.
			Regions[Pick].bSplittable = false;
			continue;
		}
		FRegion BestLeft;
		FRegion BestRight;
		int64 BestCells = Cells - FMath::Max<int64>(1, Cells / 50) + 1;
		bool bFound = false;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (int32 Cut = Regions[Pick].Lo[Axis] + 1; Cut <= Regions[Pick].Hi[Axis]; ++Cut)
			{
				FRegion Left = Regions[Pick];
				FRegion Right = Regions[Pick];
				Left.Hi[Axis] = Cut - 1;
				Right.Lo[Axis] = Cut;
				if (!Tighten(Left) || !Tighten(Right))
				{
					continue;
				}
				const int64 Together = Left.Cells() + Right.Cells();
				if (Together < BestCells)
				{
					BestCells = Together;
					BestLeft = Left;
					BestRight = Right;
					bFound = true;
				}
			}
		}
		if (!bFound)
		{
			Regions[Pick].bSplittable = false;
			continue;
		}
		Regions[Pick] = BestLeft;
		Regions.Add(BestRight);
	}

	// Each region's box is the box around the surface samples in its cells.
	for (const FRegion& Region : Regions)
	{
		FBox Around(ForceInit);
		for (int32 Z = Region.Lo[2]; Z <= Region.Hi[2]; ++Z)
		{
			for (int32 Y = Region.Lo[1]; Y <= Region.Hi[1]; ++Y)
			{
				for (int32 X = Region.Lo[0]; X <= Region.Hi[0]; ++X)
				{
					const FBox& Held = CellBox[IndexOf(X, Y, Z)];
					if (Held.IsValid != 0)
					{
						Around += Held;
					}
				}
			}
		}
		if (Around.IsValid != 0)
		{
			Result.Add(Around);
		}
	}
	return Result;
}

FMantleVehicleSkinCollision FMantleVehicleChassisSkin::ProposeCollision(const FMantleChassisSpec& Spec, const FMantleSkinnedMesh& Body, const TConstArrayView<FMantleVehiclePartSlot> Slots,
	const FMantleVehicleModel& Model, const FMantleVehicleModelPlacement& Placement, const FMantleVehicleModelMap& Map)
{
	using namespace MantleVehicleChassisSkinPrivate;
	FMantleVehicleSkinCollision Out;
	if (Body.LODs.Num() == 0 || Body.LODs[0].Positions.Num() == 0)
	{
		return Out;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	constexpr int32 MaxBoxes = FMantleVehicleLimits::MaxCollisionBoxes;

	// The chassis's own box: where the grey body collides, and the lowest any box of the model's may reach.
	const FMantleChassisPlacement Tub = FMantleVehicleChassisFit::Place(FMantleVehicleChassisBuilder::ReferenceOf(Spec), FMantleChassisStretch());
	const double Floor = Tub.CollisionCenter.Z - Tub.CollisionExtent.Z;

	// The model's surface without its moving parts (wheels, forks, propellers, rotors ride their own bones and collide as the chassis says): the triangles that follow.
	TBitArray<> Moves(false, Body.Bones.Num());
	for (const FMantleVehiclePartSlot& Slot : Slots)
	{
		if (Slot.Kind == EMantleVehiclePartKind::Surface)
		{
			continue;
		}
		const int32 Bone = Body.Bones.IndexOfByPredicate([&Slot](const FMantleSkinnedMeshBone& Candidate) { return Candidate.Name == Slot.ChassisBone; });
		if (Bone != INDEX_NONE)
		{
			Moves[Bone] = true;
		}
	}
	const auto VertexMoves = [&](const int32 Vertex)
	{
		const int32 At = Vertex * LOD.NumInfluences;
		return LOD.InfluenceBones.IsValidIndex(At) && Moves.IsValidIndex(LOD.InfluenceBones[At]) && Moves[LOD.InfluenceBones[At]];
	};
	TArray<uint32> KeptIndices;
	TArray<FVector> HullPoints;
	{
		TBitArray<> Seen(false, LOD.Positions.Num());
		for (int32 At = 0; At + 2 < LOD.Indices.Num(); At += 3)
		{
			if (VertexMoves(static_cast<int32>(LOD.Indices[At])))
			{
				continue;
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Vertex = LOD.Indices[At + Corner];
				KeptIndices.Add(Vertex);
				if (static_cast<int32>(Vertex) < LOD.Positions.Num() && !Seen[static_cast<int32>(Vertex)])
				{
					Seen[static_cast<int32>(Vertex)] = true;
					HullPoints.Add(FVector(LOD.Positions[static_cast<int32>(Vertex)]));
				}
			}
		}
	}

	// 1. The model's own collision, when it came with some: the bodies of its physics asset (a static mesh's simple shapes), but for its wheels, propellers and rotors.
	if (Model.PhysicsBodies.Num() > 0)
	{
		const TBitArray<> ModelMoves = MovingModelBones(Model, Slots, Map);
		TArray<FBox> Boxes;
		for (const FMantleVehicleModelBody& Shape : Model.PhysicsBodies)
		{
			if (Shape.Bone != INDEX_NONE && ModelMoves.IsValidIndex(Shape.Bone) && ModelMoves[Shape.Bone])
			{
				continue;
			}
			Boxes.Add(PlaceBox(Shape.Box, Placement));
		}
		const int32 Pieces = Boxes.Num();
		if (Pieces > 0)
		{
			Boxes = ReduceBoxes(MoveTemp(Boxes), MaxBoxes);
			if (ToChassisBoxes(Boxes, Floor, Tub.Seat, Out.Boxes) > 0)
			{
				Out.Source = EMantleVehicleCollisionSource::PhysicsAsset;
				Out.Note = FString::Printf(TEXT("Collision follows your model's own collision shapes: %s from %s (its wheels, propellers and rotors left out). Edit them under Tuning > Collision in the vehicle's settings."),
					*VehicleCountText(Out.Boxes.Num(), TEXT("box"), TEXT("boxes")), *VehicleCountText(Pieces, TEXT("shape"), TEXT("shapes")));
				return Out;
			}
		}
	}

	// 2. A plane or a jet without any: fuselage, wings and tail cut from the shape of the placed model.
	if (Spec.Aircraft.bSet)
	{
		const TArray<FBox> Boxes = BoxesAroundSurface(LOD.Positions, KeptIndices, MaxBoxes);
		if (Boxes.Num() > 0 && ToChassisBoxes(Boxes, Floor, Tub.Seat, Out.Boxes) > 0)
		{
			Out.Source = EMantleVehicleCollisionSource::Airframe;
			Out.Note = FString::Printf(TEXT("Collision follows the shape of your model: %s for its fuselage, wings and tail (it came with no collision shapes of its own). Edit them under Tuning > Collision in the vehicle's settings."),
				*VehicleCountText(Out.Boxes.Num(), TEXT("box"), TEXT("boxes")));
			return Out;
		}
	}

	// 3. Everything else: the chassis's box kept inside the model's length and width, with boxes over its roof and cabin, the fit a car built from its own wheels gets.
	if (HullPoints.Num() > 0)
	{
		FMantleChassisPlacement Fitted = Tub;
		FMantleVehicleChassisFit::ClampTubToBody(Fitted, HullPoints);
		Fitted.BodyBoxes = FMantleVehicleChassisFit::FitBodyBoxes(HullPoints, Fitted.CollisionCenter, Fitted.CollisionExtent);
		FMantleChassisBox& TubBox = Out.Boxes.AddDefaulted_GetRef();
		TubBox.Center = Fitted.CollisionCenter;
		TubBox.Extent = Fitted.CollisionExtent;
		for (const FMantleChassisBox& Over : Fitted.BodyBoxes)
		{
			if (Out.Boxes.Num() < MaxBoxes)
			{
				Out.Boxes.Add(Over);
			}
		}
		Out.Source = EMantleVehicleCollisionSource::Hull;
		Out.Note = Fitted.BodyBoxes.Num() > 0
			? FString::Printf(TEXT("Collision follows your model's hull: the chassis's box kept inside its length and width, with %s over its roof and cabin."), *VehicleCountText(Fitted.BodyBoxes.Num(), TEXT("box"), TEXT("boxes")))
			: FString(TEXT("Collision is the chassis's box kept inside your model's length and width; there is nothing above it to add."));
		return Out;
	}
	return Out;
}

FString FMantleVehicleChassisSkin::DescribePiece(const FMantleVehicleModel& Model, const FMantleVehicleModelPiece& Piece)
{
	switch (Piece.Kind)
	{
	case FMantleVehicleModelPiece::EKind::Bone:
		return FString::Printf(TEXT("bone %s"), *Piece.Bone.ToString());
	case FMantleVehicleModelPiece::EKind::Section:
		if (Model.LOD.Sections.IsValidIndex(Piece.Section))
		{
			// Where the piece is on the model, so a creator can tell a rotor's section from a cabin's without opening the mesh: "a small part near the top".
			TArray<int32> Vertices;
			MantleVehicleChassisSkinPrivate::PieceVertices(Model, TArray<int32>(), Piece, Vertices);
			const FBox Whole = Model.Bounds();
			FBox Part(ForceInit);
			for (const int32 Vertex : Vertices)
			{
				Part += FVector(Model.LOD.Positions[Vertex]);
			}
			FString Where;
			if (Part.IsValid && Whole.IsValid && Whole.GetSize().Z > 1.0)
			{
				const double Longest = Whole.GetSize().GetMax();
				const double Share = Part.GetSize().GetMax() / FMath::Max(Longest, 1.0);
				const double Height = (Part.GetCenter().Z - Whole.Min.Z) / Whole.GetSize().Z;
				Where = Share > 0.75 ? TEXT(", most of the model") : Share > 0.35 ? TEXT(", a big part") : TEXT(", a small part");
				if (Share <= 0.75)
				{
					Where += Height > 0.8 ? TEXT(" near the top") : Height < 0.2 ? TEXT(" near the bottom") : TEXT("");
				}
			}
			return FString::Printf(TEXT("section %d: %s (%d triangles%s)"), Piece.Section + 1, *Model.LOD.Sections[Piece.Section].MaterialName.ToString(),
				Model.LOD.Sections[Piece.Section].IndexCount / 3, *Where);
		}
		return FString::Printf(TEXT("section %d"), Piece.Section + 1);
	default:
		return TEXT("nothing");
	}
}

TArray<FMantleVehicleModelPiece> FMantleVehicleChassisSkin::Pieces(const FMantleVehicleModel& Model)
{
	TArray<FMantleVehicleModelPiece> Out;
	for (int32 Bone = 1; Bone < Model.Bones.Num(); ++Bone)
	{
		Out.Add(FMantleVehicleModelPiece::OfBone(Model.Bones[Bone].Name));
	}
	for (int32 Section = 0; Section < Model.LOD.Sections.Num(); ++Section)
	{
		Out.Add(FMantleVehicleModelPiece::OfSection(Section));
	}
	return Out;
}
