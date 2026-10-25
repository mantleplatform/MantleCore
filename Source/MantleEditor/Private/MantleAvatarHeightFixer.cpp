// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarHeightFixer.h"
#include "MantleAvatarHeight.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleBodyTopologyData.h"
#include "MantleRuntime.h"

#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "IAssetTools.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "SkeletalMeshAttributes.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarHeightFixerPrivate
{
	bool HeightIsAvatar(const FMantleConvertPart& Part)
	{
		return Part.Mesh.IsValid() && Part.Slot.Equals(TEXT("avatar"), ESearchCase::CaseSensitive);
	}
}

bool FMantleAvatarHeightFixer::MeasureHeight(const USkeletalMesh& Mesh, double& OutHeightCm, double& OutFeetZ)
{
	TArray<FVector3f> Positions;
	FMantleBodyLODTopology Topology;
	if (!UMantleBodyTopologyData::ReadLOD(&Mesh, 0, Topology, &Positions) || Positions.Num() == 0)
	{
		return false;
	}
	OutHeightCm = FMantleAvatarHeight::ZHeight(Positions);
	OutFeetZ = TNumericLimits<double>::Max();
	for (const FVector3f& Point : Positions)
	{
		OutFeetZ = FMath::Min(OutFeetZ, static_cast<double>(Point.Z));
	}
	return true;
}

double FMantleAvatarHeightFixer::ScaleFor(const double HeightCm, const double TargetCm)
{
	if (!FMath::IsFinite(HeightCm) || !FMath::IsFinite(TargetCm) || HeightCm <= 0.0 || TargetCm <= 0.0)
	{
		return 1.0;
	}
	return TargetCm / HeightCm;
}

FVector3f FMantleAvatarHeightFixer::ScalePoint(const FVector3f& Point, const double K, const double FeetZ)
{
	const FVector Feet(0.0, 0.0, FeetZ);
	return FVector3f(Feet + (FVector(Point) - Feet) * K);
}

USkeletalMesh* FMantleAvatarHeightFixer::WriteScaled(USkeletalMesh* Source, const double K, const double FeetZ, const FString& PackagePath,
	const FString& AssetName, FString& OutError)
{
	using namespace MantleAvatarHeightFixerPrivate;
	if (!Source)
	{
		OutError = TEXT("there is no mesh to resize");
		return nullptr;
	}
	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FTransform> Pose;
	FMantleAvatarRepose::ReadMesh(*Source, Names, Parents, Pose);
	// Component position' = K * position + (1 - K) * Feet: every local translation scales by K, a root's also shifts.
	const FVector Feet(0.0, 0.0, FeetZ);
	TArray<FTransform> NewPose = Pose;
	for (int32 Bone = 0; Bone < NewPose.Num(); ++Bone)
	{
		const FVector Translation = Pose[Bone].GetTranslation() * K;
		NewPose[Bone].SetTranslation(Parents[Bone] == INDEX_NONE ? Translation + Feet * (1.0 - K) : Translation);
	}
	const TArray<FTransform>& NewPoseRef = NewPose;
	USkeletalMesh* Scaled = FMantleAvatarRepose::WriteWithPose(Source, NewPose, PackagePath, AssetName,
		[&Names, &NewPoseRef, K, FeetZ](int32 /*LOD*/, FMeshDescription& Description, FString& /*OutLODError*/)
		{
			FSkeletalMeshAttributes Attributes(Description);
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
			{
				Positions.Set(Vertex, FMantleAvatarHeightFixer::ScalePoint(Positions.Get(Vertex), K, FeetZ));
			}
			for (const FName& Morph : Attributes.GetMorphTargetNames())
			{
				TVertexAttributesRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph);
				if (Deltas.IsValid())
				{
					for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
					{
						Deltas.Set(Vertex, Deltas.Get(Vertex) * static_cast<float>(K));
					}
				}
			}
			// The description's copy of the reference pose, which the next build and a reimport read.
			if (Attributes.HasBoneNameAttribute() && Attributes.HasBonePoseAttribute() && Attributes.GetNumBones() > 0)
			{
				FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
				FSkeletalMeshAttributes::FBonePoseAttributesRef Poses = Attributes.GetBonePoses();
				for (const FBoneID Bone : Attributes.Bones().GetElementIDs())
				{
					const int32 Ref = Names.IndexOfByKey(BoneNames.Get(Bone));
					if (Ref != INDEX_NONE)
					{
						Poses.Set(Bone, NewPoseRef[Ref]);
					}
				}
			}
			return true;
		}, OutError);
	if (!Scaled)
	{
		return nullptr;
	}
	// Sockets on the mesh itself sit on their bones: their offsets scale with it.
	for (USkeletalMeshSocket* Socket : Scaled->GetMeshOnlySocketList())
	{
		if (Socket)
		{
			Socket->RelativeLocation *= K;
		}
	}
	return Scaled;
}

void FMantleAvatarHeightFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	// The Convert route sets the height in its MetaHuman steps.
	if (Context.Route != EMantleAvatarRoute::AsIs)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		const FMantleConvertPart& Part = Context.Parts[PartIndex];
		double Height = 0.0;
		double FeetZ = 0.0;
		if (!MantleAvatarHeightFixerPrivate::HeightIsAvatar(Part) || !MeasureHeight(*Part.Mesh.Get(), Height, FeetZ) || Height <= 0.0)
		{
			continue;
		}
		const double Target = FMantleAvatarHeight::TargetCm();
		if (FMantleAvatarHeight::IsWithin(Height, Target, FMantleAvatarHeight::ConvertedToleranceCm))
		{
			continue;
		}
		FMantleConvertFix Fix;
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Problem = FString::Printf(TEXT("%s stands %.1f cm; every avatar is %.1f cm (+- %.1f)"), *Part.Mesh->GetName(), Height, Target,
			FMantleAvatarHeight::ConvertedToleranceCm);
		Fix.Action = FString::Printf(TEXT("scales it by %.3f about its feet, keeping its proportions"), ScaleFor(Height, Target));
		OutFixes.Add(Fix);
	}
}

bool FMantleAvatarHeightFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the height fix names no part");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh)
	{
		OutError = TEXT("the part has no mesh to resize");
		return false;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the resized mesh can go in"), *Context.OutputFolder);
		return false;
	}
	// Measured again on the mesh as it stands: an earlier fixer may have replaced it since the fix list was made.
	double Height = 0.0;
	double FeetZ = 0.0;
	if (!MeasureHeight(*Mesh, Height, FeetZ) || Height <= 0.0)
	{
		OutError = FString::Printf(TEXT("%s has no readable geometry to measure"), *Mesh->GetName());
		return false;
	}
	const double Target = FMantleAvatarHeight::TargetCm();
	if (FMantleAvatarHeight::IsWithin(Height, Target, FMantleAvatarHeight::ConvertedToleranceCm))
	{
		return true;
	}
	const double K = ScaleFor(Height, Target);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FString PackageName;
	FString AssetName;
	AssetTools.CreateUniqueAssetName(Context.OutputFolder / (Mesh->GetName() + TEXT("_Height")), FString(), PackageName, AssetName);
	USkeletalMesh* Scaled = WriteScaled(Mesh, K, FeetZ, FPackageName::GetLongPackagePath(PackageName), AssetName, OutError);
	if (!Scaled)
	{
		return false;
	}
	double After = 0.0;
	double AfterFeet = 0.0;
	if (!MeasureHeight(*Scaled, After, AfterFeet) || !FMantleAvatarHeight::IsWithin(After, Target, FMantleAvatarHeight::ConvertedToleranceCm))
	{
		OutError = FString::Printf(TEXT("%s measures %.2f cm after the resize, not %.1f cm"), *Scaled->GetName(), After, Target);
		return false;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_HEIGHT %s: %.2f cm -> %.2f cm (scale %.4f about the feet, target %.1f)"), *Mesh->GetName(), Height, After, K, Target);
	Context.Notes.Add(FString::Printf(TEXT("%s: scaled from %.1f cm to %.1f cm (the default height %.1f) as %s."), *Mesh->GetName(), Height, After, Target, *Scaled->GetName()));
	Context.CreatedAssets.Add(Scaled);
	Part.Mesh = Scaled;
	return true;
}

void RegisterMantleAvatarHeightFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarHeightFixer>());
}
