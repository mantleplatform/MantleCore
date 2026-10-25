// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarReposeFixer.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarRigMapFixer.h"
#include "MantleCosmeticExport.h"
#include "MantleRuntime.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "IAssetTools.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshTypes.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarReposePrivate
{
	int32 ReposeFindBone(TConstArrayView<FName> Names, const FString& Name)
	{
		const FName Wanted(*Name);
		for (int32 Bone = 0; Bone < Names.Num(); ++Bone)
		{
			if (Names[Bone] == Wanted)
			{
				return Bone;
			}
		}
		return INDEX_NONE;
	}

	/** CheckAvatarBodyShape's measure: the angle between Delta and the fixture's Direction as the file has it. */
	double ReposeChainDegrees(const FVector& Delta, const FVector& Direction)
	{
		const double Length = Delta.Size();
		const double Dot = FVector::DotProduct(Delta, Direction) / Length;
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0)));
	}

	bool ReposeCheckInput(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local, FString& OutError)
	{
		if (Names.Num() == 0 || Names.Num() != Parents.Num() || Names.Num() != Local.Num())
		{
			OutError = TEXT("the reference pose has no bones, or its names, parents and transforms differ in number");
			return false;
		}
		for (int32 Bone = 0; Bone < Parents.Num(); ++Bone)
		{
			if (Parents[Bone] < INDEX_NONE || Parents[Bone] >= Bone)
			{
				OutError = FString::Printf(TEXT("bone \"%s\" comes before its parent in the reference skeleton"), *Names[Bone].ToString());
				return false;
			}
		}
		return true;
	}

	/** A row-vector matrix (v * M, Unreal's convention) applied to a point and to a direction. */
	FVector ReposePoint(const FMatrix& Matrix, const FVector& Point)
	{
		return FVector(Matrix.TransformPosition(Point));
	}

	FVector ReposeDirection(const FMatrix& Matrix, const FVector& Direction)
	{
		return FVector(Matrix.TransformVector(Direction));
	}

	/** One chain to straighten: its bones and the bone turned for it (ReposePlan's doc). */
	struct FReposeStep
	{
		int32 Chain = INDEX_NONE;
		int32 From = INDEX_NONE;
		int32 To = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Depth = 0;
	};

	/** Waits for Mesh's asynchronous build, so what follows reads its finished render data and properties. */
	void ReposeFinishBuild(USkeletalMesh* Mesh)
	{
		USkinnedAsset* const Assets[] = { Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Assets);
	}

}

void FMantleAvatarRepose::ReadMesh(const USkeletalMesh& Mesh, TArray<FName>& OutNames, TArray<int32>& OutParents, TArray<FTransform>& OutLocal)
{
	const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
	const TArray<FMeshBoneInfo>& Info = Reference.GetRawRefBoneInfo();
	const TArray<FTransform>& Pose = Reference.GetRawRefBonePose();
	OutNames.Reset(Info.Num());
	OutParents.Reset(Info.Num());
	OutLocal.Reset(Info.Num());
	for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
	{
		OutNames.Add(Info[Bone].Name);
		OutParents.Add(Info[Bone].ParentIndex);
		OutLocal.Add(Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity);
	}
}

void FMantleAvatarRepose::DiscardMesh(USkeletalMesh* Mesh)
{
	if (!Mesh)
	{
		return;
	}
	FAssetRegistryModule::AssetDeleted(Mesh);
	Mesh->ClearFlags(RF_Public | RF_Standalone);
	Mesh->GetPackage()->ClearDirtyFlag();
	Mesh->MarkAsGarbage();
}

void FMantleAvatarRepose::ComposeComponent(TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local, TArray<FTransform>& OutComponent)
{
	OutComponent.SetNum(Local.Num());
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		const int32 Parent = Parents.IsValidIndex(Bone) ? Parents[Bone] : INDEX_NONE;
		OutComponent[Bone] = Parent != INDEX_NONE && Parent < Bone ? Local[Bone] * OutComponent[Parent] : Local[Bone];
	}
}

bool FMantleAvatarRepose::MeasureChains(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
	const FMantleContractSkeleton& Skeleton, TArray<FMantleReposeChain>& OutChains, FString& OutError)
{
	using namespace MantleAvatarReposePrivate;
	OutChains.Reset();
	if (!ReposeCheckInput(Names, Parents, Local, OutError))
	{
		return false;
	}
	TArray<FTransform> Component;
	ComposeComponent(Parents, Local, Component);
	for (const FMantleContractReferenceChain& Chain : Skeleton.ReferenceChains)
	{
		const int32 From = ReposeFindBone(Names, Chain.From);
		const int32 To = ReposeFindBone(Names, Chain.To);
		if (From == INDEX_NONE || To == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("the body has no bone \"%s\", so its reference pose cannot be re-posed to the %s"),
				From == INDEX_NONE ? *Chain.From : *Chain.To, *Skeleton.ReferencePoseName);
			return false;
		}
		const FVector Delta = Component[To].GetLocation() - Component[From].GetLocation();
		if (Delta.Size() < 0.01)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" sits on bone \"%s\" in the reference pose, so the chain has no direction to re-pose"),
				*Names[To].ToString(), *Names[From].ToString());
			return false;
		}
		FMantleReposeChain& Out = OutChains.AddDefaulted_GetRef();
		Out.From = Names[From];
		Out.To = Names[To];
		Out.Degrees = ReposeChainDegrees(Delta, Chain.Direction);
	}
	return true;
}

bool FMantleAvatarRepose::Plan(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
	const FMantleContractSkeleton& Skeleton, double Clean, FMantleReposePlan& OutPlan, FString& OutError)
{
	using namespace MantleAvatarReposePrivate;
	OutPlan = FMantleReposePlan();
	if (!MeasureChains(Names, Parents, Local, Skeleton, OutPlan.Before, OutError))
	{
		return false;
	}
	OutPlan.LocalPose = TArray<FTransform>(Local.GetData(), Local.Num());

	// Which bone turns for each chain, and root to leaf, so a chain's children are straightened after it.
	TArray<FReposeStep> Steps;
	for (int32 Chain = 0; Chain < Skeleton.ReferenceChains.Num(); ++Chain)
	{
		FReposeStep Step;
		Step.Chain = Chain;
		Step.From = ReposeFindBone(Names, Skeleton.ReferenceChains[Chain].From);
		Step.To = ReposeFindBone(Names, Skeleton.ReferenceChains[Chain].To);
		int32 Below = Step.To;
		while (Below != INDEX_NONE && Parents[Below] != Step.From)
		{
			Below = Parents[Below];
		}
		if (Below == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("bone \"%s\" does not hang below bone \"%s\", as it does on the %s skeleton, so the reference pose cannot be re-posed"),
				*Names[Step.To].ToString(), *Names[Step.From].ToString(), *Skeleton.Id);
			return false;
		}
		Step.Pivot = Below == Step.To ? Step.From : Below;
		for (int32 Bone = Parents[Step.Pivot]; Bone != INDEX_NONE; Bone = Parents[Bone])
		{
			++Step.Depth;
		}
		Steps.Add(Step);
	}
	Steps.StableSort([](const FReposeStep& A, const FReposeStep& B) { return A.Depth < B.Depth; });

	TArray<FTransform> Component;
	for (const FReposeStep& Step : Steps)
	{
		const FMantleContractReferenceChain& Chain = Skeleton.ReferenceChains[Step.Chain];
		ComposeComponent(Parents, OutPlan.LocalPose, Component);
		const FVector From = Component[Step.From].GetLocation();
		const FVector To = Component[Step.To].GetLocation();
		if (ReposeChainDegrees(To - From, Chain.Direction) <= Clean)
		{
			continue;
		}
		const FVector Direction = Chain.Direction.GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			continue; // A fixture chain without a direction checks nothing.
		}

		// Turn the pivot about its own origin so To lands on the ray From + t * Direction, at the distance it keeps
		// from the pivot: |From + t * Direction - Pivot| = |To - Pivot|, the far root. With the pivot at From, t is the
		// chain's length.
		const FVector Pivot = Component[Step.Pivot].GetLocation();
		const FVector Arm = To - Pivot;
		const double Reach = Arm.Size();
		const FVector Offset = From - Pivot;
		const double B = FVector::DotProduct(Offset, Direction);
		const double Discriminant = B * B - Offset.SizeSquared() + Reach * Reach;
		double T = Discriminant >= 0.0 ? -B + FMath::Sqrt(Discriminant) : -B;
		if (T <= 0.0)
		{
			T = Reach;
		}
		const FVector Target = From + Direction * T - Pivot;
		const double Turn = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Arm.GetSafeNormal(), Target.GetSafeNormal()), -1.0, 1.0)));
		if (Turn > MaxRotationDegrees)
		{
			OutError = FString::Printf(TEXT("re-posing %s to %s to the %s would turn bone \"%s\" by %.0f degrees (at most %.0f); a body that far off is probably not on the %s skeleton"),
				*Names[Step.From].ToString(), *Names[Step.To].ToString(), *Skeleton.ReferencePoseName, *Names[Step.Pivot].ToString(), Turn,
				MaxRotationDegrees, *Skeleton.Id);
			return false;
		}

		// The smallest rotation between the two (a swing: nothing about the chain), applied in component space so the
		// bone keeps its twist, then back into its parent's space. The bone stays where it is.
		const FQuat Swing = FQuat::FindBetweenVectors(Arm, Target);
		const FTransform& Old = Component[Step.Pivot];
		FTransform Turned = Old;
		Turned.SetRotation((Swing * Old.GetRotation()).GetNormalized());
		const int32 Parent = Parents[Step.Pivot];
		FTransform NewLocal = Parent != INDEX_NONE ? Turned.GetRelativeTransform(Component[Parent]) : Turned;
		NewLocal.SetTranslation(OutPlan.LocalPose[Step.Pivot].GetTranslation());
		NewLocal.SetScale3D(OutPlan.LocalPose[Step.Pivot].GetScale3D());
		OutPlan.LocalPose[Step.Pivot] = NewLocal;

		FMantleReposeRotation& Rotation = OutPlan.Rotations.AddDefaulted_GetRef();
		Rotation.Bone = Names[Step.Pivot];
		Rotation.ChainFrom = Names[Step.From];
		Rotation.ChainTo = Names[Step.To];
		Rotation.Degrees = Turn;
	}
	return MeasureChains(Names, Parents, OutPlan.LocalPose, Skeleton, OutPlan.After, OutError);
}

bool FMantleAvatarRepose::PlanForMesh(const USkeletalMesh& Mesh, const FMantleContractSkeleton& Skeleton, double Clean, FMantleReposePlan& OutPlan,
	FString& OutError)
{
	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> Local;
	FMantleAvatarRepose::ReadMesh(Mesh, Names, Parents, Local);
	return Plan(Names, Parents, Local, Skeleton, Clean, OutPlan, OutError);
}

FString FMantleAvatarRepose::DescribeChains(TConstArrayView<FMantleReposeChain> Chains, double Clean)
{
	TArray<FString> Parts;
	for (const FMantleReposeChain& Chain : Chains)
	{
		if (Chain.Degrees > Clean)
		{
			Parts.Add(FString::Printf(TEXT("%s %.0f%s"), *Chain.From.ToString(), Chain.Degrees, *FString::Chr(TCHAR(0xB0))));
		}
	}
	return FString::Join(Parts, TEXT(", "));
}

bool FMantleAvatarRepose::RebindMeshDescription(FMeshDescription& Mesh, TConstArrayView<FName> RefNames, TConstArrayView<FTransform> OldComponent,
	TConstArrayView<FTransform> NewComponent, TConstArrayView<FTransform> NewLocal, FString& OutError)
{
	using namespace MantleAvatarReposePrivate;
	const int32 NumRefBones = RefNames.Num();
	if (OldComponent.Num() != NumRefBones || NewComponent.Num() != NumRefBones || NewLocal.Num() != NumRefBones)
	{
		OutError = TEXT("the poses do not match the reference skeleton's bones");
		return false;
	}
	FSkeletalMeshAttributes Attributes(Mesh);
	FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
	if (!Weights.IsValid())
	{
		OutError = TEXT("the mesh has no skin weights to re-bind");
		return false;
	}

	// Each reference bone's skinning matrix from the old bind to the new, and whether it moves at all.
	TArray<FMatrix> Skin;
	TArray<bool> Moved;
	Skin.SetNum(NumRefBones);
	Moved.SetNum(NumRefBones);
	for (int32 Bone = 0; Bone < NumRefBones; ++Bone)
	{
		Skin[Bone] = OldComponent[Bone].ToMatrixWithScale().Inverse() * NewComponent[Bone].ToMatrixWithScale();
		Moved[Bone] = !OldComponent[Bone].Equals(NewComponent[Bone], 1.e-6);
	}

	// The description's own bones, when it names them, to reference bones by name; otherwise the same indices.
	TArray<int32> RefOfBone;
	const bool bNamedBones = Attributes.HasBoneNameAttribute() && Attributes.GetNumBones() > 0;
	if (bNamedBones)
	{
		FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
		RefOfBone.Init(INDEX_NONE, Attributes.Bones().GetArraySize());
		for (const FBoneID Bone : Attributes.Bones().GetElementIDs())
		{
			RefOfBone[Bone.GetValue()] = RefNames.IndexOfByKey(BoneNames.Get(Bone));
		}
	}
	auto RefBone = [&RefOfBone, bNamedBones, NumRefBones](const int32 Index)
	{
		const int32 Ref = bNamedBones ? (RefOfBone.IsValidIndex(Index) ? RefOfBone[Index] : INDEX_NONE) : Index;
		return Ref >= 0 && Ref < NumRefBones ? Ref : INDEX_NONE;
	};

	// Vertices: the weighted blend of their bones' skinning matrices.
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const TArray<FName> Morphs = Attributes.GetMorphTargetNames();
	TArray<TVertexAttributesRef<FVector3f>> MorphPositions;
	for (const FName& Morph : Morphs)
	{
		TVertexAttributesRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph);
		if (Deltas.IsValid())
		{
			MorphPositions.Add(Deltas);
		}
	}
	TArray<FMatrix> Blend;
	TBitArray<> VertexMoved(false, Mesh.Vertices().GetArraySize());
	Blend.SetNum(Mesh.Vertices().GetArraySize());
	for (const FVertexID Vertex : Mesh.Vertices().GetElementIDs())
	{
		FMatrix Sum(ForceInitToZero);
		double Total = 0.0;
		bool bAnyMoved = false;
		for (const UE::AnimationCore::FBoneWeight Influence : Weights.Get(Vertex))
		{
			const int32 Ref = RefBone(Influence.GetBoneIndex());
			const double Weight = Influence.GetWeight();
			if (Ref == INDEX_NONE || Weight <= 0.0)
			{
				continue;
			}
			Sum += Skin[Ref] * Weight;
			Total += Weight;
			bAnyMoved |= Moved[Ref];
		}
		if (!bAnyMoved || Total <= 0.0)
		{
			continue;
		}
		Sum *= 1.0 / Total;
		Blend[Vertex.GetValue()] = Sum;
		VertexMoved[Vertex.GetValue()] = true;
		Positions.Set(Vertex, FVector3f(ReposePoint(Sum, FVector(Positions.Get(Vertex)))));
		for (TVertexAttributesRef<FVector3f>& Deltas : MorphPositions)
		{
			Deltas.Set(Vertex, FVector3f(ReposeDirection(Sum, FVector(Deltas.Get(Vertex)))));
		}
	}

	// Vertex instances: normals and tangents turn with their vertex (the binormal sign keeps: no mirroring), and so do
	// morph normal deltas.
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TArray<TVertexInstanceAttributesRef<FVector3f>> MorphNormals;
	for (const FName& Morph : Morphs)
	{
		if (Attributes.HasMorphTargetNormalsAttribute(Morph))
		{
			TVertexInstanceAttributesRef<FVector3f> Deltas = Attributes.GetVertexInstanceMorphNormalDelta(Morph);
			if (Deltas.IsValid())
			{
				MorphNormals.Add(Deltas);
			}
		}
	}
	for (const FVertexInstanceID Instance : Mesh.VertexInstances().GetElementIDs())
	{
		const int32 Vertex = Mesh.GetVertexInstanceVertex(Instance).GetValue();
		if (!VertexMoved.IsValidIndex(Vertex) || !VertexMoved[Vertex])
		{
			continue;
		}
		const FMatrix& Sum = Blend[Vertex];
		if (Normals.IsValid())
		{
			Normals.Set(Instance, FVector3f(ReposeDirection(Sum, FVector(Normals.Get(Instance))).GetSafeNormal()));
		}
		if (Tangents.IsValid())
		{
			Tangents.Set(Instance, FVector3f(ReposeDirection(Sum, FVector(Tangents.Get(Instance))).GetSafeNormal()));
		}
		for (TVertexInstanceAttributesRef<FVector3f>& Deltas : MorphNormals)
		{
			Deltas.Set(Instance, FVector3f(ReposeDirection(Sum, FVector(Deltas.Get(Instance)))));
		}
	}

	// The description's copy of the reference pose, which the next build and a reimport read.
	if (bNamedBones && Attributes.HasBonePoseAttribute())
	{
		FSkeletalMeshAttributes::FBonePoseAttributesRef Poses = Attributes.GetBonePoses();
		for (const FBoneID Bone : Attributes.Bones().GetElementIDs())
		{
			const int32 Ref = RefBone(Bone.GetValue());
			if (Ref != INDEX_NONE)
			{
				Poses.Set(Bone, NewLocal[Ref]);
			}
		}
	}
	return true;
}

USkeletalMesh* FMantleAvatarRepose::WriteReposed(USkeletalMesh* Source, TConstArrayView<FTransform> NewLocalPose, const FString& PackagePath,
	const FString& AssetName, FString& OutError)
{
	using namespace MantleAvatarReposePrivate;
	if (!Source)
	{
		OutError = TEXT("there is no mesh to re-pose");
		return nullptr;
	}
	ReposeFinishBuild(Source);
	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> OldLocal;
	FMantleAvatarRepose::ReadMesh(*Source, Names, Parents, OldLocal);
	if (NewLocalPose.Num() != Names.Num())
	{
		OutError = FString::Printf(TEXT("the new reference pose has %d bones, %s has %d"), NewLocalPose.Num(), *Source->GetName(), Names.Num());
		return nullptr;
	}
	if (!ReposeCheckInput(Names, Parents, OldLocal, OutError))
	{
		return nullptr;
	}
	TArray<FTransform> OldComponent;
	TArray<FTransform> NewComponent;
	ComposeComponent(Parents, OldLocal, OldComponent);
	ComposeComponent(Parents, NewLocalPose, NewComponent);
	return WriteWithPose(Source, NewLocalPose, PackagePath, AssetName,
		[&Names, &OldComponent, &NewComponent, NewLocalPose](int32 /*LOD*/, FMeshDescription& Description, FString& OutLODError)
		{
			return RebindMeshDescription(Description, Names, OldComponent, NewComponent, NewLocalPose, OutLODError);
		}, OutError);
}

USkeletalMesh* FMantleAvatarRepose::WriteWithPose(USkeletalMesh* Source, TConstArrayView<FTransform> NewLocalPose, const FString& PackagePath,
	const FString& AssetName, TFunctionRef<bool(int32 LOD, FMeshDescription& Description, FString& OutError)> EditLOD, FString& OutError)
{
	using namespace MantleAvatarReposePrivate;
	if (!Source)
	{
		OutError = TEXT("there is no mesh to write");
		return nullptr;
	}
	ReposeFinishBuild(Source);
	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> OldLocal;
	FMantleAvatarRepose::ReadMesh(*Source, Names, Parents, OldLocal);
	if (NewLocalPose.Num() != Names.Num())
	{
		OutError = FString::Printf(TEXT("the new reference pose has %d bones, %s has %d"), NewLocalPose.Num(), *Source->GetName(), Names.Num());
		return nullptr;
	}

	// A copy in a new package, as the Content Browser's Duplicate makes one (its mesh descriptions, LOD settings,
	// sockets and material slots with it), never the source.
	const FString PackageName = PackagePath / AssetName;
	if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ true) || FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
	{
		OutError = FString::Printf(TEXT("%s is not a new asset path"), *PackageName);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	USkeletalMesh* Mesh = Package ? DuplicateObject<USkeletalMesh>(Source, Package, FName(*AssetName)) : nullptr;
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("could not create %s"), *PackageName);
		return nullptr;
	}
	// A copy of a transient mesh (a catalog reference body, FMantleReferenceBodies) inherits its transient flag and would
	// never be saved, or save as nothing: the copy is an asset.
	if (Source->HasAnyFlags(RF_Transient))
	{
		Mesh->ClearFlags(RF_Transient);
		ForEachObjectWithOuter(Mesh, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); }, EGetObjectsFlags::IncludeNestedObjects);
	}
	Mesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Mesh);
	ReposeFinishBuild(Mesh);
	OutError.Reset();

	// Every LOD with source geometry is re-bound in its mesh description (the 5.4+ source of truth a build reads;
	// FSkeletalMeshImportData is deprecated). A LOD the editor generates by reduction is regenerated from its base LOD
	// by the build; a LOD with only built data (an old asset) is first converted to a description. All of it before
	// anything is committed, so a LOD that cannot be re-bound leaves no half-made mesh behind.
	FSkeletalMeshModel* Imported = Mesh->GetImportedModel();
	TArray<int32> Rebound;
	for (int32 LOD = 0; LOD < Mesh->GetLODNum() && OutError.IsEmpty(); ++LOD)
	{
		FMeshDescription* Description = Mesh->HasMeshDescription(LOD) ? Mesh->GetMeshDescription(LOD) : nullptr;
		if (!Description)
		{
			const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(LOD);
			if (Info && Info->bHasBeenSimplified)
			{
				continue;
			}
			if (!Imported || !Imported->LODModels.IsValidIndex(LOD))
			{
				OutError = FString::Printf(TEXT("%s LOD %d has no geometry to re-bind"), *Source->GetName(), LOD);
				break;
			}
			FMeshDescription Converted;
			Imported->LODModels[LOD].GetMeshDescription(Mesh, LOD, Converted);
			Description = Mesh->CreateMeshDescription(LOD, MoveTemp(Converted));
		}
		FString Error;
		if (!Description || !EditLOD(LOD, *Description, Error))
		{
			OutError = FString::Printf(TEXT("%s LOD %d cannot be re-bound: %s"), *Source->GetName(), LOD, Description ? *Error : TEXT("no mesh description"));
			break;
		}
		Rebound.Add(LOD);
	}
	if (!OutError.IsEmpty())
	{
		FMantleAvatarRepose::DiscardMesh(Mesh);
		return nullptr;
	}

	{
		FScopedSkeletalMeshPostEditChange ScopedPostEditChange(Mesh);

		// The new reference pose (bone count, names and hierarchy as they were) and the inverse bind matrices of it.
		{
			FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), Mesh->GetSkeleton());
			for (int32 Bone = 0; Bone < NewLocalPose.Num(); ++Bone)
			{
				Modifier.UpdateRefPoseTransform(Bone, NewLocalPose[Bone]);
			}
		}
		Mesh->GetRefBasesInvMatrix().Reset();
		Mesh->CalculateInvRefMatrices();

		USkeletalMesh::FCommitMeshDescriptionParams Params;
		Params.bForceUpdate = true;
		for (const int32 LOD : Rebound)
		{
			Mesh->CommitMeshDescription(LOD, Params);
		}
	} // PostEditChange: the build.
	ReposeFinishBuild(Mesh);
	Mesh->MarkPackageDirty();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_AVATAR_REPOSE %s -> %s (%d bones)"), *Source->GetPathName(), *Mesh->GetPathName(), Names.Num());
	return Mesh;
}

FString FMantleAvatarRepose::ReposeWarning(TConstArrayView<FMantleReposeChain> Chains, const FString& PoseName)
{
	const FMantleReposeChain* Worst = nullptr;
	for (const FMantleReposeChain& Chain : Chains)
	{
		if (!Worst || Chain.Degrees > Worst->Degrees)
		{
			Worst = &Chain;
		}
	}
	if (!Worst || Worst->Degrees <= ReposeWarnDegrees)
	{
		return FString();
	}
	return FString::Printf(TEXT("WARNING: %s to %s is %.0f degrees off the %s (re-posed, but past %.0f degrees the result may look off): check it in the preview."),
		*Worst->From.ToString(), *Worst->To.ToString(), Worst->Degrees, *PoseName, ReposeWarnDegrees);
}

bool FMantleAvatarReposeFixer::TargetPose(const FMantleConvertContext& Context, const int32 PartIndex, const FMantleAvatarContract& Contract,
	FString& OutSkeletonId, FMantleContractSkeleton& OutPose)
{
	if (!Context.Parts.IsValidIndex(PartIndex))
	{
		return false;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	const USkeletalMesh* Mesh = Part.Mesh.Get();
	// The pose rule is an avatar body's (#582); garments and hair take the body's pose from it.
	if (!Mesh || !Part.Slot.Equals(TEXT("avatar"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	// #625: a single mesh picked as the avatar has no id yet (the upload reads it at Build): read it the upload's way.
	OutSkeletonId = Part.SkeletonId;
	if (OutSkeletonId.IsEmpty())
	{
		FString Error;
		if (!FMantleCosmeticExport::AvatarSkeletonId(Mesh, Contract, OutSkeletonId, Error))
		{
			return false;
		}
	}
	const FMantleContractSkeleton* Own = Contract.FindSkeleton(OutSkeletonId);
	if (!Own || !Own->HasReferencePose())
	{
		return false;
	}
	OutPose = *Own;
	return true;
}

void FMantleAvatarReposeFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	// #625: an avatar body is re-posed to its own skeleton's A-pose only As-is; Convert poses it into MetaHuman's A-pose.
	if (!Contract || Context.Route != EMantleAvatarRoute::AsIs)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		const FMantleConvertPart& Part = Context.Parts[PartIndex];
		const USkeletalMesh* Mesh = Part.Mesh.Get();
		FString SkeletonId;
		FMantleContractSkeleton Pose;
		if (!TargetPose(Context, PartIndex, *Contract, SkeletonId, Pose))
		{
			continue;
		}
		const FMantleContractSkeleton* Skeleton = &Pose;
		TArray<FName> Names;
		TArray<int32> Parents;
		TArray<FTransform> Local;
		FMantleAvatarRepose::ReadMesh(*Mesh, Names, Parents, Local);

		FMantleConvertFix Fix;
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		TArray<FMantleReposeChain> Chains;
		FString Error;
		if (!FMantleAvatarRepose::MeasureChains(Names, Parents, Local, *Skeleton, Chains, Error))
		{
			// A rig RigMap maps (VRoid's J_Bip_L_UpperArm, no upperarm_l yet) is measured after its bones are renamed, when
			// Convert analyses again; a line here, off, would stay off for that run.
			if (!FMantleAvatarRigMap::IsOnSupportedSkeleton(FMantleAvatarRigMap::ReadBones(Mesh), *Contract))
			{
				continue;
			}
			Fix.Problem = Error;
			Fix.Action = FString::Printf(TEXT("can't re-pose it to the %s"), *Skeleton->ReferencePoseName);
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			OutFixes.Add(Fix);
			continue;
		}
		// The upload's refusal: the first chain over the tolerance in the fixture's order, in its words.
		const double Tolerance = Contract->ChainToleranceDegrees;
		const FMantleReposeChain* Refused = Chains.FindByPredicate([Tolerance](const FMantleReposeChain& Chain) { return Chain.Degrees > Tolerance; });
		if (!Refused)
		{
			continue;
		}
		const int32 MoreRefused = Chains.FilterByPredicate([Tolerance](const FMantleReposeChain& Chain) { return Chain.Degrees > Tolerance; }).Num() - 1;
		Fix.Problem = FString::Printf(TEXT("bone \"%s\" points %.0f degrees away from the %s (%s to %s, at most %g)"),
			*Refused->From.ToString(), Refused->Degrees, *Skeleton->ReferencePoseName, *Refused->From.ToString(), *Refused->To.ToString(), Tolerance);
		if (MoreRefused > 0)
		{
			Fix.Problem += FString::Printf(TEXT(", and %d more chain%s"), MoreRefused, MoreRefused == 1 ? TEXT("") : TEXT("s"));
		}
		FMantleReposePlan Plan;
		if (!FMantleAvatarRepose::Plan(Names, Parents, Local, *Skeleton, FMantleAvatarRepose::CleanDegrees, Plan, Error))
		{
			Fix.Action = FString::Printf(TEXT("can't re-pose it: %s"), *Error);
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			OutFixes.Add(Fix);
			continue;
		}
		Fix.Action = FString::Printf(TEXT("re-poses the reference pose to the %s (%s), and re-binds the mesh"), *Skeleton->ReferencePoseName,
			*FMantleAvatarRepose::DescribeChains(Chains, FMantleAvatarRepose::CleanDegrees));
		// #625, the warn tier: past ReposeWarnDegrees it is still re-posed, with the angle named in the warning.
		const FString Warning = FMantleAvatarRepose::ReposeWarning(Chains, Skeleton->ReferencePoseName);
		if (!Warning.IsEmpty())
		{
			Fix.Action += TEXT(". ") + Warning;
		}
		OutFixes.Add(Fix);
	}
}

bool FMantleAvatarReposeFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the re-pose fix names no part");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh)
	{
		OutError = TEXT("the part has no mesh to re-pose");
		return false;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the converted mesh can go in"), *Context.OutputFolder);
		return false;
	}
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		OutError = ContractError;
		return false;
	}
	// The pose Analyze chose, decided again on the part as it stands.
	FString SkeletonId;
	FMantleContractSkeleton Pose;
	if (!TargetPose(Context, Fix.PartIndex, *Contract, SkeletonId, Pose))
	{
		OutError = FString::Printf(TEXT("the %s skeleton has no reference pose to re-pose to"), SkeletonId.IsEmpty() ? TEXT("body's") : *SkeletonId);
		return false;
	}
	if (Part.SkeletonId.IsEmpty())
	{
		Part.SkeletonId = SkeletonId;
	}
	const FMantleContractSkeleton* Skeleton = &Pose;
	// Planned again on the mesh as it stands: an earlier fixer may have replaced it since the fix list was made.
	FMantleReposePlan Plan;
	if (!FMantleAvatarRepose::PlanForMesh(*Mesh, *Skeleton, FMantleAvatarRepose::CleanDegrees, Plan, OutError))
	{
		return false;
	}
	if (Plan.Rotations.Num() == 0)
	{
		return true; // Already in the pose.
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FString PackageName;
	FString AssetName;
	AssetTools.CreateUniqueAssetName(Context.OutputFolder / (Mesh->GetName() + TEXT("_APose")), FString(), PackageName, AssetName);
	USkeletalMesh* Reposed = FMantleAvatarRepose::WriteReposed(Mesh, Plan.LocalPose, FPackageName::GetLongPackagePath(PackageName), AssetName, OutError);
	if (!Reposed)
	{
		return false;
	}

	// What the upload measures, on what was written.
	FMantleReposePlan Written;
	FString Error;
	if (!FMantleAvatarRepose::PlanForMesh(*Reposed, *Skeleton, Contract->ChainToleranceDegrees, Written, Error) || Written.Rotations.Num() > 0)
	{
		OutError = FString::Printf(TEXT("the re-posed %s is still off the %s: %s"), *Reposed->GetName(), *Skeleton->ReferencePoseName,
			Error.IsEmpty() ? *FMantleAvatarRepose::DescribeChains(Written.Before, Contract->ChainToleranceDegrees) : *Error);
		FMantleAvatarRepose::DiscardMesh(Reposed);
		return false;
	}

	TArray<FString> Turned;
	for (const FMantleReposeRotation& Rotation : Plan.Rotations)
	{
		Turned.Add(FString::Printf(TEXT("%s %.0f%s"), *Rotation.Bone.ToString(), Rotation.Degrees, *FString::Chr(TCHAR(0xB0))));
	}
	Context.Notes.Add(FString::Printf(TEXT("%s: re-posed to the %s (turned %s) and re-bound as %s."), *Mesh->GetName(), *Skeleton->ReferencePoseName,
		*FString::Join(Turned, TEXT(", ")), *Reposed->GetName()));
	// #625, the warn tier: re-posed past ReposeWarnDegrees, with the angle named in a note.
	const FString Warning = FMantleAvatarRepose::ReposeWarning(Plan.Before, Skeleton->ReferencePoseName);
	if (!Warning.IsEmpty())
	{
		Context.Notes.Add(FString::Printf(TEXT("%s: %s"), *Mesh->GetName(), *Warning));
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_REPOSE_WARN %s: %s"), *Mesh->GetName(), *Warning);
	}
	Context.CreatedAssets.Add(Reposed);
	Part.Mesh = Reposed;
	return true;
}

void RegisterMantleAvatarReposeFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarReposeFixer>());
}
