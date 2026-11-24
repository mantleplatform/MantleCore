// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size]
// Shows what a hat's hairRule does to long hair. The hair cards are fitted to the standard head as the plugin does (as
// Mantle.Dev.RenderHairFit: FMantleEditorActions::MakeCosmeticDraft, then FMantleCosmeticExport::Build); the hat is placed on the
// slot's bone of Body as Mantle.Dev.RenderAccessory places it (the panel's default placement). Then the hair is drawn three ways:
//   underIt    - the runtime's hair-under-hats step (FMantleHairHatFit::Fit on the fitted hair, the hat and the head skin): what pokes
//                through the hat is pulled in under it, what is wholly through is cut out
//   hideAll    - no hair at all
//   leaveAlone - the hair as fitted, unchanged
// each from the front, the side and the back, as the catalog thumbnail is drawn (FMantleCosmeticThumbnail, the engine's thumbnail
// scene; the head gray, the hair brown, the hat blue, all in one mesh on the engine's vertex color material), 30 frames after the
// camera is placed, into <Directory>/<Label>_{underIt,hideAll,leaveAlone}_{front,side,back}.png. Nothing is saved or uploaded.
// Logs MANTLE_RENDER_HAIRRULE lines: a start marker, the fit's notes, each picture and "done" (or "failed").
#include "MantleAccessoryPlacement.h"
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEditorActions.h"
#include "MantleHairFitPictures.h"
#include "MantleHairHatFit.h"
#include "MantlePng.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshDescription.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRenderHairRule, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevRenderHairRulePrivate
{
	template <typename T>
	T* LoadAssetByPath(FString Path)
	{
		if (!FPackageName::GetShortName(Path).Contains(TEXT("."))) Path += TEXT(".") + FPackageName::GetShortName(Path);
		return LoadObject<T>(nullptr, *Path);
	}

	/** LOD 0 of a static mesh as a triangle list, from its source mesh description (the editor keeps it). False when it has none. */
	bool ReadTriangles(const UStaticMesh* Mesh, TArray<FVector3f>& OutPositions, TArray<uint32>& OutIndices)
	{
		const FMeshDescription* Description = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
		if (!Description) return false;
		const FStaticMeshConstAttributes Attributes(*Description);
		const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		// The source geometry is before the asset's build scale; the built mesh (bounds, placement) is after it.
		const FVector3f BuildScale = Mesh->GetNumSourceModels() > 0 ? FVector3f(Mesh->GetSourceModel(0).BuildSettings.BuildScale3D) : FVector3f::OneVector;
		TMap<FVertexID, uint32> Remap;
		for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
		{
			Remap.Add(Vertex, static_cast<uint32>(OutPositions.Num()));
			OutPositions.Add(Positions[Vertex] * BuildScale);
		}
		for (const FTriangleID Triangle : Description->Triangles().GetElementIDs())
		{
			const auto Corners = Description->GetTriangleVertices(Triangle);
			for (int32 Corner = 0; Corner < 3; ++Corner) OutIndices.Add(Remap[Corners[Corner]]);
		}
		return OutIndices.Num() >= 3;
	}

	/** One static mesh of the head (gray), the hair (brown, when there is any) and the hat (blue): vertex colors on the engine's vertex color material. */
	UStaticMesh* BuildScene(const TArray<FVector3f>& HeadPositions, const TArray<uint32>& HeadIndices, const TArray<FVector3f>& HairPositions,
		const TArray<uint32>& HairIndices, const TArray<FVector3f>& HatPositions, const TArray<uint32>& HatIndices)
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UStaticMeshDescription* Description = UStaticMesh::CreateStaticMeshDescription(Mesh);
		FMeshDescription& MD = Description->GetMeshDescription();
		FStaticMeshAttributes Attributes(MD);
		Attributes.Register();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const FPolygonGroupID Group = MD.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("HairRule");

		const auto AddPart = [&](const TArray<FVector3f>& Positions, const TArray<uint32>& Triangles, const FLinearColor& Color)
		{
			TArray<FVertexID> Vertices;
			for (const FVector3f& P : Positions)
			{
				Vertices.Add(MD.CreateVertex());
				Attributes.GetVertexPositions()[Vertices.Last()] = P;
			}
			for (int32 T = 0; T + 2 < Triangles.Num(); T += 3)
			{
				TArray<FVertexInstanceID> Corners;
				for (int32 C = 0; C < 3; ++C)
				{
					const FVertexInstanceID Corner = MD.CreateVertexInstance(Vertices[Triangles[T + C]]);
					Colors[Corner] = FVector4f(Color);
					Corners.Add(Corner);
				}
				MD.CreateTriangle(Group, Corners);
			}
		};
		AddPart(HeadPositions, HeadIndices, FLinearColor(0.45f, 0.40f, 0.36f));
		AddPart(HairPositions, HairIndices, FLinearColor(0.20f, 0.09f, 0.03f));
		AddPart(HatPositions, HatIndices, FLinearColor(0.08f, 0.16f, 0.45f));

		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("HairRule")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bBuildSimpleCollision = false;
		Mesh->BuildFromMeshDescriptions({&MD}, Params);
		return Mesh;
	}

	void Run(const TArray<FString>& Args)
	{
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE start"));
		if (Args.Num() < 6)
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: usage: Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size]"));
			return;
		}
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		UStaticMesh* HatSource = LoadAssetByPath<UStaticMesh>(Args[0]);
		UStaticMesh* Hair = LoadAssetByPath<UStaticMesh>(Args[1]);
		USkeletalMesh* Body = LoadAssetByPath<USkeletalMesh>(Args[2]);
		USkeletalMesh* Face = LoadAssetByPath<USkeletalMesh>(Args[3]);
		const FString& Directory = Args[4];
		const FString& Label = Args[5];
		const FString HatSlot = Args.Num() > 6 ? Args[6] : FString(TEXT("acc_hat"));
		const int32 Size = Args.Num() > 7 ? FMath::Clamp(FCString::Atoi(*Args[7]), 128, 2048) : 768;
		FString WhyNot;
		if (!Contract || !HatSource || !Hair || !Body || !Face || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: need the contract, a hat static mesh, a hair cards static mesh, a body, a face and a renderer (%s)"), *WhyNot);
			return;
		}
		const FMantleContractSlot* Point = Contract->Slots.FindByPredicate([&HatSlot](const FMantleContractSlot& Entry) { return Entry.Id == HatSlot && Entry.Group == TEXT("accessory"); });
		if (!Point)
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: %s is not an accessory slot"), *HatSlot);
			return;
		}

		// The hair, fitted to the standard head exactly as the plugin does it (Mantle.Dev.RenderHairFit).
		const FString HairSlot = Contract->HairSlots.Num() > 0 ? Contract->HairSlots[0] : FString(TEXT("hair"));
		FMantleCosmeticDraft HairDraft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Hair, Hair, HairSlot, Label, *Contract, EMantleFitSex::Auto);
		HairDraft.AuthoringBody = Body;
		HairDraft.AuthoringFace = Face;
		HairDraft.bRenderThumbnail = false;
		const FMantleCosmeticPackage Package = FMantleCosmeticExport::Build(HairDraft, *Contract);
		for (const FString& Note : Package.Notes) UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE note: %s"), *Note);
		for (const FMantleCosmeticIssue& Issue : Package.Issues) UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE issue: %s"), *Issue.ToString());
		const FMantleHairFitMeshes& Meshes = Package.HairHeadFitMeshes;
		if (!Package.bHairHeadFitted || !Meshes.IsValid())
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: no hair fit to draw (errors=%d)"), Package.HasErrors());
			return;
		}

		// The hat where the panel's default placement puts it, on the slot's bone in Body's reference pose (Mantle.Dev.RenderAccessory).
		FMantleCosmeticDraft HatDraft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Accessory, HatSource, HatSlot, FString(), *Contract, EMantleFitSex::Auto);
		if (!HatDraft.bAttach)
		{
			FString PlacementError;
			if (!FMantleAccessorySettings::ApplyDefaultPlacement(HatDraft, Body, PlacementError, Face))
			{
				UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hat placement: no default (%s), the hat sits on its socket"), *PlacementError);
			}
		}
		FTransform Attach = FTransform::Identity;
		if (HatDraft.bAttach)
		{
			FMantleCosmeticTransform Placement;
			Placement.Rotation = FRotator(HatDraft.AttachRotation.X, HatDraft.AttachRotation.Y, HatDraft.AttachRotation.Z);
			Placement.Location = FVector(HatDraft.AttachLocation);
			Placement.Scale = FVector(HatDraft.AttachScale);
			Attach = MantleAccessoryPlacement::ClampedAttach(Placement);
		}
		const UStaticMesh* HatShown = FMantleAccessorySettings::UploadMesh(HatSource, Contract->AccessoryClassFor(HatSlot));
		FTransform Recentered = FTransform::Identity;
		if (const FMantleContractAccessoryBounds* Limits = Contract->AccessoryBounds.Find(HatSlot))
		{
			Recentered = FTransform(FVector(-FMantleAccessorySettings::RecenterOffset(FBox3f(HatShown->GetBoundingBox()), *Limits)));
		}
		const FReferenceSkeleton& Reference = Body->GetRefSkeleton();
		const TArray<FTransform>& Pose = Reference.GetRefBonePose();
		int32 Bone = Reference.FindBoneIndex(FName(*Point->SocketBone));
		if (!Pose.IsValidIndex(Bone))
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: the body has no %s bone"), *Point->SocketBone);
			return;
		}
		FTransform BoneComponent = Pose[Bone];
		while ((Bone = Reference.GetParentIndex(Bone)) != INDEX_NONE) BoneComponent = BoneComponent * Pose[Bone];
		const USkeletalMeshSocket* Own = Body->FindSocket(FName(*Point->Socket));
		const FTransform Socket = Own && Own->BoneName == FName(*Point->SocketBone) ? Own->GetSocketLocalTransform() * BoneComponent : BoneComponent;
		const FTransform Worn = Recentered * Attach * Socket;

		TArray<FVector3f> HatPositions;
		TArray<uint32> HatIndices;
		if (!ReadTriangles(HatShown, HatPositions, HatIndices))
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: the hat's triangles could not be read"));
			return;
		}
		for (FVector3f& P : HatPositions) P = FVector3f(Worn.TransformPosition(FVector(P)));

		FBox Frame(ForceInit);
		for (const FVector3f& P : Meshes.HeadPositions) Frame += FVector(P);
		for (const FVector3f& P : Meshes.AfterPositions) Frame += FVector(P);
		for (const FVector3f& P : HatPositions) Frame += FVector(P);
		FBox HatBox(ForceInit);
		for (const FVector3f& P : HatPositions) HatBox += FVector(P);
		FBox HeadBox(ForceInit);
		for (const FVector3f& P : Meshes.HeadPositions) HeadBox += FVector(P);
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hat=%s hatBounds=%s headBounds=%s"), *HatShown->GetName(), *HatBox.ToString(), *HeadBox.ToString());

		// underIt: the runtime's hair-under-hats step on the fitted hair (the hat and the head skin in the hair's space).
		FMantleSkinnedMeshLOD HairLOD;
		HairLOD.Positions = Meshes.AfterPositions;
		HairLOD.Indices = Meshes.HairTriangles;
		FMantleSkinnedMeshSection Section;
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = HairLOD.Indices.Num();
		HairLOD.Sections.Add(Section);
		FMantleHairHatFitParams Params;
		Params.Rule = EMantleHairHatRule::UnderIt;
		Params.HeadCentre = FMantleHairHatFit::HeadCentreFromSkin(Meshes.HeadPositions, Meshes.HeadTriangles);
		FMantleHairHatFitResult Fitted;
		FMantleHairHatFit::Fit(HairLOD, HatPositions, HatIndices, Meshes.HeadPositions, Meshes.HeadTriangles, Params, Fitted);
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE underIt: %d points pulled in, %d of %d triangles cut out (%.1f ms)"),
			Fitted.PointsPulled, Fitted.TrianglesDropped, Meshes.HairTriangles.Num() / 3, Fitted.Ms);
		TSet<int32> Dropped(Fitted.DroppedTriangles);
		TArray<uint32> TuckedIndices;
		for (int32 Triangle = 0; Triangle * 3 + 2 < Meshes.HairTriangles.Num(); ++Triangle)
		{
			if (!Dropped.Contains(Triangle)) TuckedIndices.Append(&Meshes.HairTriangles[Triangle * 3], 3);
		}

		struct FRule { const TCHAR* Name; const TArray<FVector3f>* Positions; const TArray<uint32>* Indices; };
		const TArray<FVector3f> NoPositions;
		const TArray<uint32> NoIndices;
		const FRule Rules[] = { { TEXT("underIt"), &Fitted.Positions, &TuckedIndices }, { TEXT("hideAll"), &NoPositions, &NoIndices },
			{ TEXT("leaveAlone"), &Meshes.AfterPositions, &Meshes.HairTriangles } };
		struct FView { const TCHAR* Name; float Yaw; };
		const FView Views[] = { { TEXT("front"), FMantleHairFitPictures::FrontYaw }, { TEXT("side"), FMantleHairFitPictures::SideYaw }, { TEXT("back"), FMantleHairFitPictures::BackYaw } };
		int32 Failed = 0;
		for (const FRule& Rule : Rules)
		{
			TStrongObjectPtr<UStaticMesh> Scene(BuildScene(Meshes.HeadPositions, Meshes.HeadTriangles, *Rule.Positions, *Rule.Indices, HatPositions, HatIndices));
			for (const FView& View : Views)
			{
				FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::AccessorySubject(Scene.Get(), FQuat::Identity, true);
				Shot.Frame = Frame;
				Shot.View = FRotator(0.0f, View.Yaw, 0.0f);
				// 30 frames after the camera is placed, before the picture is taken.
				Shot.SettleFrames = 30;
				TArray<FColor> Pixels;
				FString Error;
				const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s_%s.png"), *Label, Rule.Name, View.Name));
				const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
					&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
				Failed += bWritten ? 0 : 1;
				UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE %s %s %s %s"), Rule.Name, View.Name, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
			}
		}
		if (Failed > 0)
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: %d of %d pictures"), Failed, static_cast<int32>(UE_ARRAY_COUNT(Rules) * UE_ARRAY_COUNT(Views)));
			return;
		}
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE done"));
	}

	FAutoConsoleCommand GMantleDevRenderHairRuleCommand(
		TEXT("Mantle.Dev.RenderHairRule"),
		TEXT("Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size] -- draws a hat worn over long hair three ways ")
		TEXT("(underIt, hideAll, leaveAlone), each from the front, side and back, 30 frames after the camera is placed. Saves and uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
