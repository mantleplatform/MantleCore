// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBuiltInMannequin.h"

#include "MantleCosmeticExport.h"
#include "MantleRuntime.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleSkinnedMesh.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigSkeleton.h"
#include "UObject/SoftObjectPath.h"

const TCHAR* const FMantleBuiltInMannequin::RigPath = TEXT("/MantleCore/Avatars/Retarget/IK_UEFN_Mannequin.IK_UEFN_Mannequin");

namespace MantleBuiltInMannequinPrivate
{
	/** The built mesh, rooted once made: it lives as long as the editor. */
	USkeletalMesh* Built = nullptr;
}

bool FMantleBuiltInMannequin::MakeMesh(TConstArrayView<FName> Bones, TConstArrayView<int32> Parents, TConstArrayView<FTransform> RefPoseGlobal,
	FMantleSkinnedMesh& Out, FString& OutError)
{
	Out = FMantleSkinnedMesh();
	if (Bones.Num() == 0 || Parents.Num() != Bones.Num() || RefPoseGlobal.Num() != Bones.Num())
	{
		OutError = FString::Printf(TEXT("the rig's skeleton has %d bones, %d parents and %d reference transforms"), Bones.Num(), Parents.Num(), RefPoseGlobal.Num());
		return false;
	}
	for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
	{
		const bool bParentFirst = Bone == 0 ? Parents[Bone] == INDEX_NONE : (Parents[Bone] >= 0 && Parents[Bone] < Bone);
		if (!bParentFirst)
		{
			OutError = FString::Printf(TEXT("the rig's bone %d (%s) has parent %d: the skeleton is not parent-first with one root"), Bone, *Bones[Bone].ToString(), Parents[Bone]);
			return false;
		}
		Out.Bones.Add({ Bones[Bone], Parents[Bone] });
		FTransform Local = Parents[Bone] == INDEX_NONE ? RefPoseGlobal[Bone] : RefPoseGlobal[Bone].GetRelativeTransform(RefPoseGlobal[Parents[Bone]]);
		Local.NormalizeRotation();
		Out.ReferencePose.Add(Local);
	}

	// One tiny triangle on the root, never drawn: the engine animates only a mesh with render data.
	FMantleSkinnedMeshLOD& LOD = Out.LODs.AddDefaulted_GetRef();
	LOD.Positions = { FVector3f(0.0f, 0.0f, 0.0f), FVector3f(0.1f, 0.0f, 0.0f), FVector3f(0.0f, 0.1f, 0.0f) };
	LOD.Normals = { FVector3f::UpVector, FVector3f::UpVector, FVector3f::UpVector };
	LOD.UVs = { FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(0.0f, 1.0f) };
	LOD.Indices = { 0, 1, 2 };
	LOD.NumInfluences = 1;
	LOD.InfluenceBones = { 0, 0, 0 };
	LOD.InfluenceWeights = { 255, 255, 255 };
	FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
	Section.IndexCount = 3;
	return Out.Validate(OutError);
}

USkeletalMesh* FMantleBuiltInMannequin::Get(FString* OutError)
{
	using namespace MantleBuiltInMannequinPrivate;
	if (Built)
	{
		return Built;
	}
	auto Refuse = [OutError](const FString& Reason) -> USkeletalMesh*
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_BUILTIN_MANNEQUIN failed: %s"), *Reason);
		if (OutError)
		{
			*OutError = Reason;
		}
		return nullptr;
	};
	const FSoftObjectPath Path(RigPath);
	const UIKRigDefinition* Rig = FPackageName::DoesPackageExist(Path.GetLongPackageName()) ? Cast<UIKRigDefinition>(Path.TryLoad()) : nullptr;
	if (!Rig)
	{
		return Refuse(FString::Printf(TEXT("the Mantle plugin's IK rig %s is missing or not an IK rig"), *Path.GetAssetName()));
	}
	const FIKRigSkeleton& Skeleton = Rig->GetSkeleton();
	FMantleSkinnedMesh Mesh;
	FString Error;
	if (!MakeMesh(Skeleton.BoneNames, Skeleton.ParentIndices, Skeleton.RefPoseGlobal, Mesh, Error))
	{
		return Refuse(FString::Printf(TEXT("the Mantle plugin's IK rig %s stores no usable skeleton (%s)"), *Path.GetAssetName(), *Error));
	}
	FMantleSkinnedMeshBuildParams Params;
	Params.bOwnSkeleton = true;
	Params.MaxLODs = 1;
	Params.Name = TEXT("SKM_Mantle_BuiltIn_UEFN_Mannequin");
	USkeletalMesh* Made = FMantleRuntimeSkeletalMeshBuilder::Build(Mesh, Params, &Error);
	if (!Made)
	{
		return Refuse(FString::Printf(TEXT("a mesh could not be made from %s (%s)"), *Path.GetAssetName(), *Error));
	}
	// Its skeleton is reached from the mesh, so rooting the mesh keeps both.
	Made->AddToRoot();
	Built = Made;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_BUILTIN_MANNEQUIN built %s from %s: %d bones"), *Made->GetPathName(), RigPath, Mesh.Bones.Num());
	return Built;
}

bool FMantleBuiltInMannequin::Is(const USkeletalMesh* Mesh)
{
	return Mesh && Mesh == MantleBuiltInMannequinPrivate::Built;
}

USkeletalMesh* FMantleBuiltInMannequin::UEFNMannequin(const FMantleAvatarContract& Contract, FString* OutError)
{
	if (USkeletalMesh* Project = FMantleCosmeticExport::FindUEFNMannequin(Contract))
	{
		return Project;
	}
	return Get(OutError);
}
