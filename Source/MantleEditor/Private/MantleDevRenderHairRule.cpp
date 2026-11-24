// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size] [rule=a|b]
// Shows what a hat's hairRule does to long hair, as a player wearing both sees it. The hair is exported as the upload makes it, on the
// head it was authored on (FMantleHairSourceDiscovery::FindHead: for MetaHuman's own grooms SKM_Groom_Head_Legacy01; else Face), then
// moved onto Face, the wearer's, by the client's own step (FMantleHairHeadFit::Fit, MetaHuman UV correspondence); the head drawn is
// Face's head skin (FMantleHairHeadFit::HeadSkin). The hat is on Body's socket bone with the panel's placement (a saved one, else the
// default), as Mantle.Dev.RenderAccessory places it. Then the hair is drawn three ways:
//   underIt    - the runtime's hair-under-hats step (FMantleHairHatFit::Fit on the fitted hair, the hat and the head skin): what pokes
//                through the hat is pulled in under it, what is wholly through is cut out
//   hideAll    - no hair at all
//   leaveAlone - the hair as fitted, unchanged
// each from the front, the side and the back, as the catalog thumbnail is drawn (FMantleCosmeticThumbnail, the engine's thumbnail
// scene; the head gray, the hair brown, the hat blue: one mesh, one section per part, each on its own flat color of the engine's
// BasicShapeMaterial), 30 frames after the
// camera is placed, into <Directory>/<Label>_{underIt_<rule>,hideAll,leaveAlone}_{front,side,back}.png. rule=a (the default, the game's) or rule=b picks
// what underIt aims at (FMantleHairHatFitParams::Aim: hair poking through the hat, or whole cards mostly under it), for Joshua's choice 1. Nothing is saved or uploaded.
// Logs MANTLE_RENDER_HAIRRULE lines: a start marker, the fit's notes, each picture and "done" (or "failed").
#include "MantleAccessoryPlacement.h"
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEditorActions.h"
#include "MantleHairFitPictures.h"
#include "MantleHairHatFit.h"
#include "MantleHairHeadFit.h"
#include "MantleHairSourceDiscovery.h"
#include "MantlePng.h"
#include "MantleSkinnedMesh.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
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

	void Run(const TArray<FString>& AllArgs)
	{
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE start"));
		// rule=a (the game's: what pokes through is pulled in, a triangle goes when it is wholly through) or rule=b (whole cards mostly
		// under the hat hidden; FMantleHairHatFitParams::Aim, EMantleHairHatAim), anywhere after the six required arguments.
		EMantleHairHatAim Aim = EMantleHairHatAim::PokingThrough;
		TArray<FString> Args;
		for (const FString& Arg : AllArgs)
		{
			if (Arg.Equals(TEXT("rule=a"), ESearchCase::IgnoreCase)) Aim = EMantleHairHatAim::PokingThrough;
			else if (Arg.Equals(TEXT("rule=b"), ESearchCase::IgnoreCase)) Aim = EMantleHairHatAim::UnderHatCards;
			else Args.Add(Arg);
		}
		const TCHAR* RuleName = Aim == EMantleHairHatAim::UnderHatCards ? TEXT("b") : TEXT("a");
		if (Args.Num() < 6)
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: usage: Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size] [rule=a|b]"));
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

		// The hair as the upload makes it, on the head it was authored on: found as the upload finds it (FindHead; MetaHuman's own
		// grooms name SKM_Groom_Head_Legacy01 in their binding), else Face (bought cards, fitted to it by the export).
		FString HeadError;
		USkeletalMesh* HairHead = FMantleHairSourceDiscovery::FindHead(Hair, HeadError);
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hair authored on: %s (%s)"), *(HairHead ? HairHead : Face)->GetPathName(),
			HairHead ? TEXT("its recorded head") : *FString::Printf(TEXT("the face given; %s"), *HeadError));
		const FString HairSlot = Contract->HairSlots.Num() > 0 ? Contract->HairSlots[0] : FString(TEXT("hair"));
		FMantleCosmeticDraft HairDraft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Hair, Hair, HairSlot, Label, *Contract, EMantleFitSex::Auto);
		HairDraft.AuthoringBody = HairHead ? HairHead : Body;
		HairDraft.AuthoringFace = HairHead ? HairHead : Face;
		HairDraft.bRenderThumbnail = false;
		FMantleSkinnedMesh Cards, SourceHead;
		const FMantleCosmeticPackage Package = FMantleCosmeticExport::Build(HairDraft, *Contract, &Cards, &SourceHead);
		for (const FString& Note : Package.Notes) UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE note: %s"), *Note);
		for (const FMantleCosmeticIssue& Issue : Package.Issues) UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE issue: %s"), *Issue.ToString());
		if (Package.HasErrors() || Cards.LODs.IsEmpty() || SourceHead.LODs.IsEmpty())
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: the hair export made no cards and source head (errors=%d)"), Package.HasErrors());
			return;
		}

		// Then as players' clients wear it: moved from its authoring head onto the wearer's face by MetaHuman UV correspondence
		// (FMantleHairHeadFit::Fit, the same step the client and the panel preview run), with the hat on the wearer's own head bone.
		// The authoring head itself is never worn: Legacy01 is a different, lower head than the standard face (its head top about
		// 12 cm below it), so a hat on Legacy01's head bone floated over its hair.
		FMantleSkinnedMesh WearerFace, WearerBody, Worn;
		FString FitError;
		if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Face, WearerFace, &FitError) || !FMantleSkinnedMesh::ExtractFromSkeletalMesh(Body, WearerBody, &FitError)
			|| !FMantleHairHeadFit::Fit(Cards, SourceHead, WearerFace, WearerBody, false, Worn, FitError) || Worn.LODs.IsEmpty())
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: the hair could not be fitted to the wearer's face (%s)"), *FitError);
			return;
		}
		FMantleHairFitMeshes Meshes;
		Meshes.AfterPositions = Worn.LODs[0].Positions;
		Meshes.HairTriangles = Worn.LODs[0].Indices;
		if (!FMantleHairHeadFit::HeadSkin(WearerFace, Meshes.HeadPositions, Meshes.HeadTriangles))
		{
			UE_LOG(LogMantleRenderHairRule, Error, TEXT("MANTLE_RENDER_HAIRRULE failed: the wearer's face has no head skin"));
			return;
		}
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE hair fitted to %s: %d hair and %d head triangles"), *Face->GetName(),
			Meshes.HairTriangles.Num() / 3, Meshes.HeadTriangles.Num() / 3);

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
		// The hat on the wearer's body, on its socket bone (the socket sits at identity there), as the client attaches it.
		const USkeletalMesh* HatOn = Body;
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
		Params.Aim = Aim;
		Params.HeadCentre = FMantleHairHatFit::HeadCentreFromSkin(Meshes.HeadPositions, Meshes.HeadTriangles);
		FMantleHairHatFitResult Fitted;
		FMantleHairHatFit::Fit(HairLOD, HatPositions, HatIndices, Meshes.HeadPositions, Meshes.HeadTriangles, Params, Fitted);
		UE_LOG(LogMantleRenderHairRule, Display, TEXT("MANTLE_RENDER_HAIRRULE underIt rule %s: %d points pulled in, %d of %d triangles cut out, %d cards hidden whole (%.1f ms)"),
			RuleName, Fitted.PointsPulled, Fitted.TrianglesDropped, Meshes.HairTriangles.Num() / 3, Fitted.CardsHidden, Fitted.Ms);
		TSet<int32> Dropped(Fitted.DroppedTriangles);
		TArray<uint32> TuckedIndices;
		for (int32 Triangle = 0; Triangle * 3 + 2 < Meshes.HairTriangles.Num(); ++Triangle)
		{
			if (!Dropped.Contains(Triangle)) TuckedIndices.Append(&Meshes.HairTriangles[Triangle * 3], 3);
		}

		struct FRule { const TCHAR* Name; const TArray<FVector3f>* Positions; const TArray<uint32>* Indices; };
		const TArray<FVector3f> NoPositions;
		const TArray<uint32> NoIndices;
		const FString UnderItName = FString::Printf(TEXT("underIt_%s"), RuleName);
		const FRule Rules[] = { { *UnderItName, &Fitted.Positions, &TuckedIndices }, { TEXT("hideAll"), &NoPositions, &NoIndices },
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
		TEXT("Mantle.Dev.RenderHairRule <HatStaticMesh> <HairCardsMesh> <Body> <Face> <Directory> <Label> [slot=acc_hat] [size] [rule=a|b] -- draws a hat worn over long hair three ways ")
		TEXT("(underIt, hideAll, leaveAlone), each from the front, side and back, 30 frames after the camera is placed. Saves and uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
