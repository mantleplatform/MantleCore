// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAccessorySettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "MantleRuntime.h"
#include "MantleAvatarContract.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Engine/StaticMesh.h"
#include "MantleMeshReduce.h"
#include "MantlePlaceAssets.h"
#include "StaticMeshResources.h"

namespace MantleAccessorySettingsPrivate
{
	FString Section(const FMantleCosmeticDraft& Draft)
	{
		return FString::Printf(TEXT("Mantle.AccessoryPlacement.%s.%s"), *Draft.Source->GetPathName(), *Draft.Slot);
	}
}

FString FMantleAccessorySettings::DefaultConfigFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("MantleAccessoryPlacement.ini")));
}

bool FMantleAccessorySettings::Load(FMantleCosmeticDraft& Draft, const FString& ConfigFile)
{
	if (Draft.Class != EMantleCosmeticClass::Accessory || !Draft.Source.IsValid()) return false;
	const FString Section = MantleAccessorySettingsPrivate::Section(Draft);
	Draft.bAttach = false;
	Draft.AttachLocation = FVector3f::ZeroVector;
	Draft.AttachRotation = FVector3f::ZeroVector;
	Draft.AttachScale = FVector3f::OneVector;
	FConfigFile Saved;
	Saved.Read(ConfigFile);
	Saved.GetBool(*Section, TEXT("Enabled"), Draft.bAttach);
	FVector Value;
	FString Text;
	if (Saved.GetString(*Section, TEXT("Location"), Text) && Value.InitFromString(Text) && !Value.ContainsNaN()) Draft.AttachLocation = FMantleCosmeticPackage::ClampAttachLocation(FVector3f(Value));
	if (Saved.GetString(*Section, TEXT("Rotation"), Text) && Value.InitFromString(Text) && !Value.ContainsNaN()) Draft.AttachRotation = FMantleCosmeticPackage::ClampAttachRotation(FVector3f(Value));
	if (Saved.GetString(*Section, TEXT("Scale"), Text) && Value.InitFromString(Text) && !Value.ContainsNaN()) Draft.AttachScale = FMantleCosmeticPackage::ClampAttachScale(FVector3f(Value));
	return Saved.Contains(Section);
}

bool FMantleAccessorySettings::AlignToAvatarAxes(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError)
{
	OutError.Reset();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&OutError);
	const FMantleContractSlot* Point = Contract ? Contract->Slots.FindByPredicate([&Draft](const FMantleContractSlot& Entry)
		{ return Entry.Id == Draft.Slot && Entry.Group == TEXT("accessory"); }) : nullptr;
	FString SkeletonId;
	if (Draft.Class != EMantleCosmeticClass::Accessory || !Body || !Point
		|| !FMantleCosmeticExport::AvatarSkeletonId(Body, *Contract, SkeletonId, OutError) || SkeletonId != TEXT("metahuman"))
	{
		if (OutError.IsEmpty()) OutError = TEXT("Choose a MetaHuman body and an accessory slot to align its axes.");
		return false;
	}
	const FReferenceSkeleton& Reference = Body->GetRefSkeleton();
	const TArray<FTransform>& Pose = Reference.GetRefBonePose();
	int32 Bone = Reference.FindBoneIndex(FName(*Point->SocketBone));
	if (!Pose.IsValidIndex(Bone))
	{
		OutError = FString::Printf(TEXT("The chosen MetaHuman has no %s attachment bone."), *Point->SocketBone);
		return false;
	}
	FTransform Component = Pose[Bone];
	while ((Bone = Reference.GetParentIndex(Bone)) != INDEX_NONE) Component = Component * Pose[Bone];
	FQuat Rotation = Component.GetRotation().Inverse();
	Rotation.Normalize();
	if (Rotation.ContainsNaN())
	{
		OutError = TEXT("The chosen MetaHuman's reference attachment rotation is invalid.");
		return false;
	}
	const FRotator Angles = Rotation.Rotator();
	Draft.AttachRotation = FMantleCosmeticPackage::ClampAttachRotation(FVector3f(Angles.Pitch, Angles.Yaw, Angles.Roll));
	Draft.bAttach = true;
	return true;
}

void FMantleAccessorySettings::Save(const FMantleCosmeticDraft& Draft, const FString& ConfigFile)
{
	if (Draft.Class != EMantleCosmeticClass::Accessory || !Draft.Source.IsValid()) return;
	// A dedicated plain ini avoids config-branch command-stream/diff saving and preserves other
	// source/slot entries. The editor's shared settings branch does not own this file.
	FConfigFile Saved;
	Saved.Read(ConfigFile);
	Saved.bCanSaveAllSections = true;
	const FString Section = MantleAccessorySettingsPrivate::Section(Draft);
	Saved.SetBool(*Section, TEXT("Enabled"), Draft.bAttach);
	Saved.SetString(*Section, TEXT("Location"), *FVector(Draft.AttachLocation).ToString());
	Saved.SetString(*Section, TEXT("Rotation"), *FVector(Draft.AttachRotation).ToString());
	Saved.SetString(*Section, TEXT("Scale"), *FVector(Draft.AttachScale).ToString());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ConfigFile), true);
	if (!Saved.Write(ConfigFile, false)) UE_LOG(LogMantle, Warning, TEXT("Accessory placement could not be saved to %s"), *ConfigFile);
}

FVector3f FMantleAccessorySettings::RecenterOffset(const FBox3f& Bounds, const FMantleContractAccessoryBounds& Limits)
{
	if (!Bounds.IsValid) return FVector3f::ZeroVector;
	const FVector3f Size = Bounds.GetSize();
	if (Size.GetMax() > Limits.MaxDimensionCm) return FVector3f::ZeroVector;
	const double Reach = FVector3f(FMath::Max(FMath::Abs(Bounds.Min.X), FMath::Abs(Bounds.Max.X)),
		FMath::Max(FMath::Abs(Bounds.Min.Y), FMath::Abs(Bounds.Max.Y)), FMath::Max(FMath::Abs(Bounds.Min.Z), FMath::Abs(Bounds.Max.Z))).Size();
	if (Reach <= Limits.MaxReachCm) return FVector3f::ZeroVector;
	// Centred, the farthest corner is half the box's diagonal away.
	return Size.Size() * 0.5 <= Limits.MaxReachCm ? Bounds.GetCenter() : FVector3f::ZeroVector;
}

const UStaticMesh* FMantleAccessorySettings::UploadMesh(const UStaticMesh* Source, const FMantleContractClass& Class)
{
	if (!Source || Class.MaxTriangles <= 0) return Source;
	const FStaticMeshRenderData* Render = Source->GetRenderData();
	if (!Render || Render->LODResources.Num() == 0 || Render->LODResources[0].GetNumTriangles() <= Class.MaxTriangles) return Source;
	FMantlePlaceAssetLimits Limits;
	Limits.MaxTriangles = Class.MaxTriangles;
	FString Why;
	const UStaticMesh* Copy = FMantleMeshReduce::ReducedCopy(Source, Limits, Why);
	return Copy ? Copy : Source;
}
