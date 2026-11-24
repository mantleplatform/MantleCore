// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size]
// Shows what a hat's hairRule does to long hair. The hair goes on the head it was authored on, found as the upload finds it
// (FMantleHairSourceDiscovery::FindHead: a recorded head, else the binding's SourceSkeletalMesh of the groom that uses the cards; for
// MetaHuman's own grooms that is SKM_Groom_Head_Legacy01, not the standard face), else on Face. Bought cards are fitted to that head as
// the plugin does (as Mantle.Dev.RenderHairFit: FMantleEditorActions::MakeCosmeticDraft, then FMantleCosmeticExport::Build); cards the
// export keeps where they are are drawn as they are on that head's skin. The hat is placed on the slot's bone of that head (of Body when
// the head has no such bone) as Mantle.Dev.RenderAccessory places it (the panel's default placement). Then the hair is drawn three ways:
//   underIt    - the runtime's hair-under-hats step (FMantleHairHatFit::Fit on the fitted hair, the hat and the head skin): what pokes
//                through the hat is pulled in under it, what is wholly through is cut out
//   hideAll    - no hair at all
//   leaveAlone - the hair as fitted, unchanged
// each from the front, the side and the back, as the catalog thumbnail is drawn (FMantleCosmeticThumbnail, the engine's thumbnail
// scene; the head gray, the hair brown, the hat blue: one mesh, one section per part, each on its own flat color of the engine's
// BasicShapeMaterial), 30 frames after the
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
#include "MantleHairSourceDiscovery.h"
#include "MantlePng.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
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

	/**
	 * The face's skin as a triangle list in its reference pose, from its render data (as the export reads it): LOD 3 when there is one
	 * (the LOD the hair export authors on), else its last. Only sections whose slot reads as skin or head; all of them when none does.
	 */
	bool ReadHeadSkin(const USkeletalMesh* Face, TArray<FVector3f>& OutPositions, TArray<uint32>& OutIndices)
	{
		const FSkeletalMeshRenderData* RenderData = Face ? Face->GetResourceForRendering() : nullptr;
		if (!RenderData || RenderData->LODRenderData.IsEmpty()) return false;
		const int32 LODIndex = FMath::Min(3, RenderData->LODRenderData.Num() - 1);
		const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
		const FPositionVertexBuffer& PositionBuffer = LOD.StaticVertexBuffers.PositionVertexBuffer;
		TArray<uint32> Indices;
		LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
		const TArray<FSkeletalMaterial>& Slots = Face->GetMaterials();
		const FSkeletalMeshLODInfo* LODInfo = Face->GetLODInfo(LODIndex);
		TArray<uint32> Skin, All;
		for (int32 SectionIndex = 0; SectionIndex < LOD.RenderSections.Num(); ++SectionIndex)
		{
			const FSkelMeshRenderSection& Section = LOD.RenderSections[SectionIndex];
			int32 Slot = Section.MaterialIndex;
			if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(SectionIndex) && Slots.IsValidIndex(LODInfo->LODMaterialMap[SectionIndex])) Slot = LODInfo->LODMaterialMap[SectionIndex];
			const FString Name = Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName.ToString().ToLower() : FString();
			const bool bSkin = Name.Contains(TEXT("skin")) || Name.Contains(TEXT("head"));
			for (uint32 I = 0; I < Section.NumTriangles * 3; ++I)
			{
				const uint32 Index = Indices[Section.BaseIndex + I];
				All.Add(Index);
				if (bSkin) Skin.Add(Index);
			}
		}
		const TArray<uint32>& Chosen = Skin.IsEmpty() ? All : Skin;
		OutPositions.SetNumUninitialized(PositionBuffer.GetNumVertices());
		for (uint32 Vertex = 0; Vertex < PositionBuffer.GetNumVertices(); ++Vertex) OutPositions[Vertex] = PositionBuffer.VertexPosition(Vertex);
		OutIndices = Chosen;
		return OutIndices.Num() >= 3;
	}

	/**
	 * One static mesh of the head (gray), the hair (brown, when there is any) and the hat (blue): one section per part, each on its own
	 * flat color of the engine's BasicShapeMaterial (its Color parameter, as the builder viewport uses it). The engine's vertex color
	 * material drew all three near white in the thumbnail scene. Empty parts get no section.
	 */
	UStaticMesh* BuildScene(const TArray<FVector3f>& HeadPositions, const TArray<uint32>& HeadIndices, const TArray<FVector3f>& HairPositions,
		const TArray<uint32>& HairIndices, const TArray<FVector3f>& HatPositions, const TArray<uint32>& HatIndices)
	{
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UStaticMeshDescription* Description = UStaticMesh::CreateStaticMeshDescription(Mesh);
		FMeshDescription& MD = Description->GetMeshDescription();
		FStaticMeshAttributes Attributes(MD);
		Attributes.Register();
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

		const auto AddPart = [&](const TArray<FVector3f>& Positions, const TArray<uint32>& Triangles, const FLinearColor& Color, const TCHAR* Name)
		{
			if (Triangles.Num() < 3) return;
			const FPolygonGroupID Group = MD.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(Name);
			UMaterialInstanceDynamic* Flat = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
			Flat->SetVectorParameterValue(TEXT("Color"), Color);
			Mesh->GetStaticMaterials().Add(FStaticMaterial(Flat, FName(Name), FName(Name)));
			TArray<FVertexID> Vertices;
			for (const FVector3f& P : Positions)
			{
				Vertices.Add(MD.CreateVertex());
				Attributes.GetVertexPositions()[Vertices.Last()] = P;
			}
			for (int32 T = 0; T + 2 < Triangles.Num(); T += 3)
			{
				TArray<FVertexInstanceID> Corners;
				for (int32 C = 0; C < 3; ++C) Corners.Add(MD.CreateVertexInstance(Vertices[Triangles[T + C]]));
				MD.CreateTriangle(Group, Corners);
			}
		};
		AddPart(HeadPositions, HeadIndices, FLinearColor(0.45f, 0.40f, 0.36f), TEXT("Head"));
		AddPart(HairPositions, HairIndices, FLinearColor(0.20f, 0.09f, 0.03f), TEXT("Hair"));
		AddPart(HatPositions, HatIndices, FLinearColor(0.08f, 0.16f, 0.45f), TEXT("Hat"));

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

		// The head the hair was authored on, found as the upload finds it. MetaHuman's own grooms name theirs in their binding
		// (SourceSkeletalMesh: SKM_Groom_Head_Legacy01), and their cards sit on that head, not on the standard face; drawing them on
		// Face put them 11 cm low. Hair with no such head (bought cards) is fitted to Face, as the upload does with the face chosen.
		FString HeadError;
		USkeletalMesh* HairHead = FMantleHairSourceDiscovery::FindHead(Hair, HeadError);
		USkeletalMesh* DrawnHead = HairHead ? HairHead : Face;
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hair head: %s (%s)"), *DrawnHead->GetPathName(),
			HairHead ? TEXT("the head the hair was authored on") : *FString::Printf(TEXT("the face given; %s"), *HeadError));

		// The hair on that head exactly as the plugin does it (Mantle.Dev.RenderHairFit).
		const FString HairSlot = Contract->HairSlots.Num() > 0 ? Contract->HairSlots[0] : FString(TEXT("hair"));
		FMantleCosmeticDraft HairDraft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Hair, Hair, HairSlot, Label, *Contract, EMantleFitSex::Auto);
		HairDraft.AuthoringBody = HairHead ? HairHead : Body;
		HairDraft.AuthoringFace = DrawnHead;
		HairDraft.bRenderThumbnail = false;
		const FMantleCosmeticPackage Package = FMantleCosmeticExport::Build(HairDraft, *Contract);
		for (const FString& Note : Package.Notes) UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE note: %s"), *Note);
		for (const FMantleCosmeticIssue& Issue : Package.Issues) UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE issue: %s"), *Issue.ToString());
		// Fitted hair: the fit's own geometry. Hair the export keeps where it is (MetaHuman's own cards): the cards as they are, on the
		// skin of the head they were authored on, both in that head's reference pose.
		FMantleHairFitMeshes Meshes = Package.HairHeadFitMeshes;
		if (!Package.bHairHeadFitted || !Meshes.IsValid())
		{
			Meshes = FMantleHairFitMeshes();
			if (Package.HasErrors() || !ReadTriangles(Hair, Meshes.AfterPositions, Meshes.HairTriangles) || !ReadHeadSkin(DrawnHead, Meshes.HeadPositions, Meshes.HeadTriangles))
			{
				UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: no hair to draw (errors=%d)"), Package.HasErrors());
				return;
			}
			UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hair kept as it is (not refitted): %d hair and %d head triangles"),
				Meshes.HairTriangles.Num() / 3, Meshes.HeadTriangles.Num() / 3);
		}

		// The hat where the panel's default placement puts it (Mantle.Dev.RenderAccessory).
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
		// The hat goes on the slot's bone of the head the hair is drawn on, so hat, hair and head share one space; on Body's when that
		// head has no such bone.
		const USkeletalMesh* HatOn = DrawnHead->GetRefSkeleton().FindBoneIndex(FName(*Point->SocketBone)) != INDEX_NONE ? DrawnHead : Body;
		const FReferenceSkeleton& Reference = HatOn->GetRefSkeleton();
		const TArray<FTransform>& Pose = Reference.GetRefBonePose();
		int32 Bone = Reference.FindBoneIndex(FName(*Point->SocketBone));
		if (!Pose.IsValidIndex(Bone))
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: %s has no %s bone"), *HatOn->GetName(), *Point->SocketBone);
			return;
		}
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hat on %s's %s bone"), *HatOn->GetName(), *Point->SocketBone);
		FTransform BoneComponent = Pose[Bone];
		while ((Bone = Reference.GetParentIndex(Bone)) != INDEX_NONE) BoneComponent = BoneComponent * Pose[Bone];
		const USkeletalMeshSocket* Own = HatOn->FindSocket(FName(*Point->Socket));
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
		FBox HairBox(ForceInit);
		for (const FVector3f& P : Meshes.AfterPositions) HairBox += FVector(P);
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hat=%s hatBounds=%s headBounds=%s hairBounds=%s"), *HatShown->GetName(), *HatBox.ToString(), *HeadBox.ToString(), *HairBox.ToString());

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
