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
#include "Rendering/SkeletalMeshRenderData.h"

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

bool FMantleAccessorySettings::ApplyDefaultPlacement(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError, const USkeletalMesh* Face)
{
	if (!AlignToAvatarAxes(Draft, Body, OutError))
	{
		return false;
	}
	if (Draft.Slot.Contains(TEXT("neck")))
	{
		FString Ignored;
		RestOnShoulders(Draft, Body, Ignored, Face);
	}
	return true;
}

namespace MantleAccessorySettingsPrivate
{
	/** LOD 0 of a skinned mesh as triangle corners in its reference pose (the shape the picture draws). */
	void AppendSkin(const USkeletalMesh* Mesh, const double MinZ, const double MaxZ, TArray<FVector>& OutTriangles)
	{
		const FSkeletalMeshRenderData* Data = Mesh ? Mesh->GetResourceForRendering() : nullptr;
		if (!Data || Data->LODRenderData.IsEmpty()) return;
		const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
		const FPositionVertexBuffer& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
		const FRawStaticIndexBuffer16or32Interface* Indices = LOD.MultiSizeIndexContainer.GetIndexBuffer();
		if (!Indices) return;
		for (int32 Index = 0; Index + 2 < Indices->Num(); Index += 3)
		{
			const FVector Corner[3] = { FVector(Positions.VertexPosition(Indices->Get(Index))), FVector(Positions.VertexPosition(Indices->Get(Index + 1))),
				FVector(Positions.VertexPosition(Indices->Get(Index + 2))) };
			const double Low = FMath::Min3(Corner[0].Z, Corner[1].Z, Corner[2].Z);
			const double High = FMath::Max3(Corner[0].Z, Corner[1].Z, Corner[2].Z);
			if (High > MinZ && Low < MaxZ) OutTriangles.Append(Corner, 3);
		}
	}
}

double FMantleAccessorySettings::DropOntoSurface(TConstArrayView<FVector> Links, TConstArrayView<FVector> Triangles, const double ClearanceCm, const double MaxDropCm)
{
	double Best = -1.0;
	for (const FVector& Link : Links)
	{
		double Below = -TNumericLimits<double>::Max();
		for (int32 Index = 0; Index + 2 < Triangles.Num(); Index += 3)
		{
			const FVector& A = Triangles[Index];
			const FVector& B = Triangles[Index + 1];
			const FVector& C = Triangles[Index + 2];
			// The link's vertical line through the triangle: barycentric weights in the XY plane.
			const double Area = (B.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (B.Y - A.Y);
			if (FMath::Abs(Area) < UE_DOUBLE_SMALL_NUMBER) continue;
			const double U = ((B.X - Link.X) * (C.Y - Link.Y) - (C.X - Link.X) * (B.Y - Link.Y)) / Area;
			const double V = ((C.X - Link.X) * (A.Y - Link.Y) - (A.X - Link.X) * (C.Y - Link.Y)) / Area;
			const double W = 1.0 - U - V;
			if (U < 0.0 || V < 0.0 || W < 0.0) continue;
			const double Surface = U * A.Z + V * B.Z + W * C.Z;
			if (Surface <= Link.Z + ClearanceCm) Below = FMath::Max(Below, Surface);
		}
		if (Below == -TNumericLimits<double>::Max()) continue;
		const double Drop = FMath::Max(0.0, Link.Z - Below - ClearanceCm);
		if (Drop <= MaxDropCm && (Best < 0.0 || Drop < Best)) Best = Drop;
	}
	return Best;
}

bool FMantleAccessorySettings::RestOnShoulders(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError, const USkeletalMesh* Face)
{
	OutError.Reset();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&OutError);
	const FMantleContractSlot* Point = Contract ? Contract->Slots.FindByPredicate([&Draft](const FMantleContractSlot& Entry)
		{ return Entry.Id == Draft.Slot && Entry.Group == TEXT("accessory"); }) : nullptr;
	const UStaticMesh* Mesh = Cast<UStaticMesh>(Draft.Source.Get());
	if (!Point || !Body || !Mesh)
	{
		return false;
	}
	Mesh = UploadMesh(Mesh, Contract->RigidAccessory);
	const FReferenceSkeleton& Reference = Body->GetRefSkeleton();
	const TArray<FTransform>& Pose = Reference.GetRefBonePose();
	int32 Bone = Reference.FindBoneIndex(FName(*Point->SocketBone));
	const FStaticMeshRenderData* MeshData = Mesh->GetRenderData();
	if (!Pose.IsValidIndex(Bone) || !MeshData || MeshData->LODResources.IsEmpty())
	{
		OutError = TEXT("The neck bone or the accessory's vertices could not be read.");
		return false;
	}
	FTransform SocketBone = Pose[Bone];
	while ((Bone = Reference.GetParentIndex(Bone)) != INDEX_NONE) SocketBone = SocketBone * Pose[Bone];
	// The mesh as it is worn now (attach rotation and offset on the bone), in the body's component space, whose Z is up.
	const FTransform Worn = FTransform(FRotator(Draft.AttachRotation.X, Draft.AttachRotation.Y, Draft.AttachRotation.Z), FVector(Draft.AttachLocation), FVector(Draft.AttachScale)) * SocketBone;
	const FPositionVertexBuffer& MeshPositions = MeshData->LODResources[0].VertexBuffers.PositionVertexBuffer;
	TArray<FVector> Links;
	double Low = TNumericLimits<double>::Max(), High = -TNumericLimits<double>::Max();
	for (uint32 Index = 0; Index < MeshPositions.GetNumVertices(); ++Index)
	{
		Links.Add(Worn.TransformPosition(FVector(MeshPositions.VertexPosition(Index))));
		Low = FMath::Min(Low, Links.Last().Z);
		High = FMath::Max(High, Links.Last().Z);
	}
	// What hangs from the loop (a pendant) rests on the chest; the upper half of the mesh is what lies on the shoulders.
	const double Split = High - (High - Low) * 0.5;
	Links.RemoveAll([Split](const FVector& Position) { return Position.Z < Split; });
	// The skin the picture draws: the body and the face (the neck belongs to both), under and around the chain.
	TArray<FVector> Skin;
	MantleAccessorySettingsPrivate::AppendSkin(Body, Low - 40.0, High + 5.0, Skin);
	MantleAccessorySettingsPrivate::AppendSkin(Face, Low - 40.0, High + 5.0, Skin);
	constexpr double ClearanceCm = 0.15, MaxDropCm = 30.0;
	const double Drop = Links.IsEmpty() || Skin.IsEmpty() ? -1.0 : DropOntoSurface(Links, Skin, ClearanceCm, MaxDropCm);
	if (Drop < 0.0)
	{
		OutError = TEXT("There is no neck or shoulder skin under the chain to rest it on.");
		return false;
	}
	const FVector InBone = SocketBone.GetRotation().Inverse().RotateVector(FVector(0.0, 0.0, -Drop));
	Draft.AttachLocation = FMantleCosmeticPackage::ClampAttachLocation(Draft.AttachLocation + FVector3f(InBone));
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
	// Centerd, the farthest corner is half the box's diagonal away.
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
