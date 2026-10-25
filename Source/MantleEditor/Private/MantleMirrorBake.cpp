// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMirrorBake.h"

#include "MantleMeshReduce.h"
#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMirrorBakePrivate
{
	/** Bumped when what a mirrored copy holds changes, so a cached copy from before is not reused. */
	const TCHAR* const BakeVersion = TEXT("mirrorbake-1");

	struct FCopy
	{
		TWeakObjectPtr<const UStaticMesh> Source;

		/** What the copy was made from (see Signature); a different one means the source changed. */
		FString Signature;
		TObjectPtr<UStaticMesh> Mesh;
	};

	/** Holds every mirrored copy (they are transient, and nothing else references them) until its source changes. */
	class FMirrorCache : public FGCObject
	{
	public:
		TMap<FString, FCopy> Copies;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			for (TPair<FString, FCopy>& Entry : Copies)
			{
				Collector.AddReferencedObject(Entry.Value.Mesh);
			}
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("FMantleMirrorBakeCache");
		}
	};

	FMirrorCache& Cache()
	{
		// Never destroyed: a static destructor would run after the object system is gone.
		static FMirrorCache* Instance = new FMirrorCache();
		return *Instance;
	}

	/** -1 on each axis Scale has below zero, 1 on the others. */
	FVector SignsOf(const FVector& Scale)
	{
		return FVector(Scale.X < 0.0 ? -1.0 : 1.0, Scale.Y < 0.0 ? -1.0 : 1.0, Scale.Z < 0.0 ? -1.0 : 1.0);
	}

	/** The negative axes by name: "X", "YZ", "XYZ". */
	FString AxesOf(const FVector& Signs)
	{
		FString Axes;
		Axes += Signs.X < 0.0 ? TEXT("X") : TEXT("");
		Axes += Signs.Y < 0.0 ? TEXT("Y") : TEXT("");
		Axes += Signs.Z < 0.0 ? TEXT("Z") : TEXT("");
		return Axes;
	}

	/**
	 * The slot name every polygon group and material slot of a copy is given, by the source's slot index, so the
	 * engine's build maps each section back to the slot it drew with (it matches them by this name).
	 */
	const TCHAR* const SlotPrefix = TEXT("MantleMirrorSlot_");

	FName SlotName(int32 Index)
	{
		return FMantleMeshReduce::SlotName(SlotPrefix, Index);
	}

	/**
	 * Everything about the source a copy depends on: its geometry (the lighting guid changes whenever the mesh is
	 * rebuilt) and its material slots (assigning a slot's material does not).
	 */
	FString SignatureOf(const UStaticMesh& Source)
	{
		FString Text = FString::Printf(TEXT("%s|%s"), BakeVersion, *Source.GetLightingGuid().ToString());
		for (const FStaticMaterial& Slot : Source.GetStaticMaterials())
		{
			Text += FString::Printf(TEXT("|%s/%s/%s"), *Slot.MaterialSlotName.ToString(), *Slot.ImportedMaterialSlotName.ToString(),
				Slot.MaterialInterface ? *Slot.MaterialInterface->GetPathName() : TEXT(""));
		}
		return Text;
	}

	/**
	 * The rotation of a shape after its space is mirrored: M R M for a mirror M. Reflecting a rotation keeps its
	 * angle and turns its axis into the reflected axis times the mirror's determinant (an axis is a pseudovector).
	 */
	FRotator MirrorRotation(const FRotator& Rotation, const FVector& Signs)
	{
		const FQuat Quat = Rotation.Quaternion();
		const double Determinant = Signs.X * Signs.Y * Signs.Z;
		return FQuat(Determinant * Signs.X * Quat.X, Determinant * Signs.Y * Quat.Y, Determinant * Signs.Z * Quat.Z, Quat.W).GetNormalized().Rotator();
	}
}

bool FMantleMirrorBake::HasNegativeAxis(const FVector& Scale)
{
	return Scale.X < 0.0 || Scale.Y < 0.0 || Scale.Z < 0.0;
}

bool FMantleMirrorBake::FlipsWinding(const FVector& Scale)
{
	return ((Scale.X < 0.0 ? 1 : 0) + (Scale.Y < 0.0 ? 1 : 0) + (Scale.Z < 0.0 ? 1 : 0)) % 2 == 1;
}

void FMantleMirrorBake::MirrorCollision(const UBodySetup& From, UBodySetup& To, const FVector& Scale)
{
	using namespace MantleMirrorBakePrivate;
	const FVector Signs = SignsOf(Scale);

	To.CollisionTraceFlag = From.CollisionTraceFlag;
	To.AggGeom.EmptyElements();

	To.AggGeom.BoxElems = From.AggGeom.BoxElems;
	for (FKBoxElem& Box : To.AggGeom.BoxElems)
	{
		Box.Center *= Signs;
		Box.Rotation = MirrorRotation(Box.Rotation, Signs);
	}
	To.AggGeom.SphereElems = From.AggGeom.SphereElems;
	for (FKSphereElem& Sphere : To.AggGeom.SphereElems)
	{
		Sphere.Center *= Signs;
	}
	To.AggGeom.SphylElems = From.AggGeom.SphylElems;
	for (FKSphylElem& Capsule : To.AggGeom.SphylElems)
	{
		Capsule.Center *= Signs;
		Capsule.Rotation = MirrorRotation(Capsule.Rotation, Signs);
	}
	To.AggGeom.TaperedCapsuleElems = From.AggGeom.TaperedCapsuleElems;
	for (FKTaperedCapsuleElem& Capsule : To.AggGeom.TaperedCapsuleElems)
	{
		Capsule.Center *= Signs;
		Capsule.Rotation = MirrorRotation(Capsule.Rotation, Signs);
	}
	// A hull is mirrored in the mesh's space: its own transform applied to its points first, then reset, so a hull
	// transform never needs mirroring itself. Its index data went with the old winding, and its cooked shape with it.
	for (const FKConvexElem& Source : From.AggGeom.ConvexElems)
	{
		FKConvexElem Hull = Source;
		const FTransform Transform = Source.GetTransform();
		for (FVector& Point : Hull.VertexData)
		{
			Point = Transform.TransformPosition(Point) * Signs;
		}
		Hull.SetTransform(FTransform::Identity);
		Hull.IndexData.Reset();
		Hull.ResetChaosConvexMesh();
		Hull.UpdateElemBox();
		To.AggGeom.ConvexElems.Add(MoveTemp(Hull));
	}
	To.InvalidatePhysicsData();
}

UStaticMesh* FMantleMirrorBake::MirroredCopy(UStaticMesh* Source, const FVector& Scale, FVector& OutPositiveScale, FString& OutWhy)
{
	using namespace MantleMirrorBakePrivate;
	OutPositiveScale = Scale.GetAbs();
	OutWhy.Reset();
	if (!HasNegativeAxis(Scale))
	{
		OutWhy = TEXT("the scale has no negative axis, so there is nothing to mirror");
		return nullptr;
	}
	if (!IsValid(Source))
	{
		OutWhy = TEXT("there is no mesh to mirror");
		return nullptr;
	}

	const FVector Signs = SignsOf(Scale);
	const FString Axes = AxesOf(Signs);
	FMirrorCache& Store = Cache();

	// A copy whose source is gone is not needed again.
	for (auto It = Store.Copies.CreateIterator(); It; ++It)
	{
		if (!It.Value().Source.IsValid())
		{
			It.RemoveCurrent();
		}
	}

	const FString Key = Source->GetPathName() + TEXT("|") + Axes;
	const FString Signature = SignatureOf(*Source);
	if (const FCopy* Found = Store.Copies.Find(Key))
	{
		if (Found->Source.Get() == Source && Found->Signature == Signature && IsValid(Found->Mesh))
		{
			return Found->Mesh;
		}
	}

	const FStaticMeshRenderData* RenderData = Source->GetRenderData();
	if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
	{
		OutWhy = FString::Printf(TEXT("the mesh '%s' has no render data"), *Source->GetName());
		return nullptr;
	}

	// Every LOD with vertices an export can read, as the converter reads them (the first that can't is where the copy stops).
	TArray<FMeshDescription> Descriptions;
	const int32 MaxLODs = FMath::Min(RenderData->LODResources.Num(), static_cast<int32>(MAX_STATIC_MESH_LODS));
	const FTransform Mirror(FQuat::Identity, FVector::ZeroVector, Signs);
	for (int32 LODIndex = 0; LODIndex < MaxLODs; ++LODIndex)
	{
		// One polygon group per section, named by the section's slot index (the reader shared with the mesh reduction).
		FMeshDescription Part;
		const FMantleMeshReduce::EReadResult Read = FMantleMeshReduce::ReadRenderLOD(*Source, LODIndex, SlotPrefix, Part);
		if (Read != FMantleMeshReduce::EReadResult::Read)
		{
			if (LODIndex == 0)
			{
				OutWhy = Read == FMantleMeshReduce::EReadResult::Unreadable
					? FString::Printf(TEXT("the mesh '%s' has no readable geometry (turn on Allow CPU Access in its details)"), *Source->GetName())
					: FString::Printf(TEXT("the mesh '%s' has no triangles"), *Source->GetName());
				return nullptr;
			}
			break;
		}

		// The renderer's own normal transform, as a mirrored placed mesh draws. For a mirror this moves the vertices, mirrors
		// normals and tangents, flips the binormal sign and reverses the triangles when an odd number of axes is mirrored.
		FStaticMeshOperations::ApplyTransform(Part, Mirror, /*bApplyCorrectNormalTransform*/ false);
		Descriptions.Add(MoveTemp(Part));
	}

	UObject* Outer = GetTransientPackage();
	const FString MeshName = FString::Printf(TEXT("MantleMirror_%s_%s_%s"), *Source->GetName().Left(60), *Axes, *FMD5::HashAnsiString(*Signature).Left(8));
	UStaticMesh* Baked = NewObject<UStaticMesh>(Outer, FMantlePlaceAssets::UniqueBakedMeshName(Outer, MeshName), RF_Transient);

	// The source's slots in the source's order, so every slot index (and a component's override of it) means the same thing.
	for (const FStaticMaterial& Slot : Source->GetStaticMaterials())
	{
		FStaticMaterial& Copied = Baked->GetStaticMaterials().Add_GetRef(Slot);
		Copied.ImportedMaterialSlotName = SlotName(Baked->GetStaticMaterials().Num() - 1);
	}

	Baked->bAllowCPUAccess = true;
	Baked->SetLightMapCoordinateIndex(Source->GetLightMapCoordinateIndex());
	Baked->SetLightMapResolution(Source->GetLightMapResolution());
	Baked->SetNumSourceModels(Descriptions.Num());
	for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
	{
		// SetNumSourceModels gives a new LOD that has no mesh description yet a 50% reduction of the LOD before it, and the build
		// would then replace this LOD's own description with that reduction. Every LOD here has its own: reduce none.
		Baked->GetSourceModel(LODIndex).ReductionSettings = FMeshReductionSettings();
		FMeshBuildSettings& Settings = Baked->GetSourceModel(LODIndex).BuildSettings;
		if (LODIndex < Source->GetNumSourceModels())
		{
			// The precision the source stores its tangents and UVs at: a mirrored copy is as exact as the original.
			Settings.bUseHighPrecisionTangentBasis = Source->GetSourceModel(LODIndex).BuildSettings.bUseHighPrecisionTangentBasis;
			Settings.bUseFullPrecisionUVs = Source->GetSourceModel(LODIndex).BuildSettings.bUseFullPrecisionUVs;
		}
		// The geometry is final: nothing recomputed (the mirrored normals, tangents and lightmap UVs are kept as they are),
		// and no distance field or Lumen cards, which only an editor view of the mesh would use.
		Settings.bRecomputeNormals = false;
		Settings.bRecomputeTangents = false;
		Settings.bGenerateLightmapUVs = false;
		Settings.bBuildReversedIndexBuffer = false;
		Settings.DistanceFieldResolutionScale = 0.0f;
		Settings.MaxLumenMeshCards = 0;
	}

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = true;
	TArray<const FMeshDescription*> Pointers;
	for (const FMeshDescription& Description : Descriptions)
	{
		Pointers.Add(&Description);
	}
	if (!Baked->BuildFromMeshDescriptions(Pointers, Params))
	{
		OutWhy = FString::Printf(TEXT("the engine could not build a static mesh from the mirrored '%s'"), *Source->GetName());
		return nullptr;
	}
	FStaticMeshRenderData* BakedData = Baked->GetRenderData();
	if (!BakedData || !BakedData->LODResources.IsValidIndex(0) || BakedData->LODResources[0].GetNumTriangles() <= 0)
	{
		OutWhy = FString::Printf(TEXT("the mirrored '%s' has no triangles"), *Source->GetName());
		return nullptr;
	}

	// The LODs switch where the source's do (the build gives its own, by LOD number).
	for (int32 LODIndex = 0; LODIndex < BakedData->LODResources.Num() && LODIndex < MAX_STATIC_MESH_LODS; ++LODIndex)
	{
		BakedData->ScreenSize[LODIndex].Default = RenderData->ScreenSize[LODIndex].Default;
	}

	// Its collision: the source's shapes mirrored, or the triangles where the source has no body setup.
	Baked->CreateBodySetup();
	if (UBodySetup* Body = Baked->GetBodySetup())
	{
		if (const UBodySetup* SourceBody = Source->GetBodySetup())
		{
			MirrorCollision(*SourceBody, *Body, Scale);
		}
		else
		{
			Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Body->InvalidatePhysicsData();
		}
	}

	UE_LOG(LogMantle, Log, TEXT("MANTLE_628 MirrorBake: '%s' mirrored on %s into '%s' (%d LOD%s, %d triangles)"), *Source->GetName(), *Axes, *Baked->GetName(),
		Descriptions.Num(), Descriptions.Num() == 1 ? TEXT("") : TEXT("s"), BakedData->LODResources[0].GetNumTriangles());

	FCopy& Stored = Store.Copies.FindOrAdd(Key);
	Stored.Source = Source;
	Stored.Signature = Signature;
	Stored.Mesh = Baked;
	return Baked;
}

void FMantleMirrorBake::ClearCache()
{
	MantleMirrorBakePrivate::Cache().Copies.Empty();
}
