// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayWorn.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleOverlayChecks.h"
#include "MantleOverlayBake.h"
#include "SMantleOverlayPlaceViewport.h"
#include "MantleReferenceBodies.h"
#include "MantleRuntimeTextureBuilder.h"

#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MantleRuntime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

bool FMantleOverlayWorn::Make(const FMantleCosmeticPackage& Built, const FString& Slot, const FMantleAvatarContract& Contract, USkeletalMesh* PickedBody,
	USkeletalMesh* PickedFace, FMantleOverlayWorn& Out, FString& OutRefusal)
{
	Out = FMantleOverlayWorn();
	const FMantleCosmeticUploadAsset* MaskAsset = Built.MaskKey.IsEmpty() ? nullptr : Built.FindAsset(Built.MaskKey);
	if (!MaskAsset)
	{
		OutRefusal = TEXT("Choose the overlay's mask first.");
		return false;
	}
	FString Error;
	FMantleTextureBuildParams MaskParams;
	MaskParams.Role = EMantleTextureRole::Data;
	MaskParams.MaxDimension = FMantleOverlayChecks::MaxSize;
	MaskParams.AddressX = MaskParams.AddressY = TA_Clamp;
	MaskParams.LODGroup = TEXTUREGROUP_Character;
	UTexture2D* MaskTexture = FMantleRuntimeTextureBuilder::BuildFromBytes(MaskAsset->Bytes, MaskParams, &Error);
	if (!MaskTexture)
	{
		OutRefusal = FString::Printf(TEXT("The mask could not be loaded for the preview: %s"), *Error);
		return false;
	}
	Out.KeepAlive.Add(TStrongObjectPtr<UObject>(MaskTexture));
	UTexture2D* NormalTexture = nullptr;
	if (const FMantleCosmeticUploadAsset* NormalAsset = Built.OverlayNormalKey.IsEmpty() ? nullptr : Built.FindAsset(Built.OverlayNormalKey))
	{
		FMantleTextureBuildParams NormalParams = MaskParams;
		NormalParams.Role = EMantleTextureRole::Normal;
		NormalTexture = FMantleRuntimeTextureBuilder::BuildFromBytes(NormalAsset->Bytes, NormalParams, &Error);
		if (!NormalTexture)
		{
			OutRefusal = FString::Printf(TEXT("The normal map could not be loaded for the preview: %s"), *Error);
			return false;
		}
		Out.KeepAlive.Add(TStrongObjectPtr<UObject>(NormalTexture));
	}

	// The converted body and face: the picked ones, else Mantle's reference body and face.
	Out.bFace = FMantleOverlayChecks::IsFaceSlot(Slot);
	USkeletalMesh* Body = PickedBody;
	USkeletalMesh* Face = PickedFace;
	if (!Body)
	{
		FString WhyNot;
		const TArray<USkeletalMesh*> References = FMantleReferenceBodies::FindBodies(Contract, &WhyNot);
		Body = References.IsEmpty() ? nullptr : References[0];
		if (!Body)
		{
			OutRefusal = FString::Printf(TEXT("There is no converted MetaHuman body to preview on: %s Choose a converted body under Preview body."), *WhyNot);
			return false;
		}
	}
	if (!Face)
	{
		Face = FMantleReferenceBodies::GetFace();
	}
	if (Out.bFace && !Face)
	{
		OutRefusal = TEXT("A face overlay needs a converted MetaHuman face to preview on. Choose one under Preview face.");
		return false;
	}
	Out.Body = Body;
	Out.Face = Face;

	// The shared skin materials the game draws overlays on. They are project content, so a project without them shows the mesh as it is.
	USkeletalMesh* Target = Out.bFace ? Face : Body;
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, Out.bFace
		? TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedFace.MI_MantleSharedFace")
		: TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedBody.MI_MantleSharedBody"));
	if (!Parent)
	{
		Out.Note = TEXT(" The shared skin material (/Game/Avatars/Appearance/Shared) is not in this project, so the mask is not drawn; the mesh shows as it is.");
		return true;
	}
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
	Out.KeepAlive.Add(TStrongObjectPtr<UObject>(Material));
	const bool bScar = FMantleOverlayChecks::IsScarSlot(Slot);
	const TCHAR* MaskName = bScar ? TEXT("ScarMask") : TEXT("TattooMask");
	UTexture* Existing = nullptr;
	if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(MaskName)), Existing))
	{
		Out.Note = FString::Printf(TEXT(" The shared skin material has no %s parameter, so the mask is not drawn."), MaskName);
		return true;
	}
	Material->SetTextureParameterValue(FName(MaskName), MaskTexture);
	if (bScar)
	{
		if (NormalTexture)
		{
			Material->SetTextureParameterValue(TEXT("ScarNormal"), NormalTexture);
		}
		Material->SetScalarParameterValue(TEXT("ScarStrength"), 1.0f);
		Material->SetVectorParameterValue(TEXT("ScarColor"), FLinearColor(0.35f, 0.12f, 0.10f));
		Material->SetScalarParameterValue(TEXT("ScarRoughness"), 0.5f);
	}
	else
	{
		Material->SetScalarParameterValue(TEXT("TattooStrength"), 1.0f);
		Material->SetVectorParameterValue(TEXT("TattooColor"), FLinearColor(0.02f, 0.02f, 0.05f));
	}
	// The skin sections only: eyes, teeth and lashes keep their own materials.
	const TArray<FString> Names = FMantleCosmeticExport::SectionMaterialNames(Target, 0);
	const TArray<FString> Roles = FMantleCosmeticExport::DefaultSectionRoles(Target, 0);
	const TArray<FSkeletalMaterial>& Slots = Target->GetMaterials();
	TMap<int32, UMaterialInterface*>& Applied = Out.bFace ? Out.FaceMaterials : Out.BodyMaterials;
	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		const int32 Section = Names.IndexOfByKey(Slots[SlotIndex].MaterialSlotName.ToString());
		if (Roles.IsValidIndex(Section) && Roles[Section] == TEXT("skin"))
		{
			Applied.Add(SlotIndex, Material);
		}
	}
	if (Applied.IsEmpty() && Slots.Num() == 1)
	{
		Applied.Add(0, Material);
	}
	if (Applied.IsEmpty())
	{
		Out.Note = TEXT(" Could not tell which part of the mesh is the skin, so the mask is not drawn.");
	}
	Out.bDrawn = !Applied.IsEmpty();
	return true;
}

FMantleThumbnailSubject FMantleOverlayWorn::ToSubject() const
{
	FMantleThumbnailSubject Subject = FMantleCosmeticThumbnail::AvatarSubject(Body, Face);
	if (!bFace && Body)
	{
		// A body overlay: the whole standing body, not just the head and shoulders, with room round it (the imported bounds are a little tight).
		FBox Whole = Body->GetImportedBounds().GetBox();
		Whole = Whole.ExpandBy(Whole.GetExtent() * 0.12);
		Subject.Frame = Whole;
		Subject.Describe.ReplaceInline(TEXT(", headshot"), TEXT(", full body"));
	}
	Subject.MaterialOverrides.Add(BodyMaterials);
	if (Subject.SkeletalMeshes.Num() > 1)
	{
		Subject.MaterialOverrides.Add(FaceMaterials);
	}
	for (const TStrongObjectPtr<UObject>& Held : KeepAlive)
	{
		Subject.KeepAlive.Add(Held);
	}
	Subject.Describe += FString::Printf(TEXT(", wearing the %s overlay"), bFace ? TEXT("face") : TEXT("body"));
	return Subject;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOverlayWornDevCommands
{
	/**
	 * #124: an overlay's thumbnail in each choice, through the panel's own export (Build): Automatic (the standard body wearing the mark, the catalog capture),
	 * Choose the angle (from behind and to the right) and a Custom image. One PNG each in Directory; saves and uploads nothing.
	 */
	void RunOverlaySamples(const TArray<FString>& Args)
	{
		if (Args.Num() < 4)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_OVERLAY_SAMPLES usage: Mantle.Dev.OverlayThumbnailSamples <Directory> <Mask.png> <Slot> <Custom.png> [Exit]"));
			return;
		}
		IFileManager::Get().MakeDirectory(*Args[0], true);
		FString Error;
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&Error);
		if (!Contract)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_OVERLAY_SAMPLES failed: %s"), *Error);
			return;
		}
		struct FSample { const TCHAR* Name; EMantleThumbnailMode Mode; };
		for (const FSample& Sample : { FSample{ TEXT("automatic"), EMantleThumbnailMode::Automatic }, FSample{ TEXT("angle"), EMantleThumbnailMode::Angle },
			FSample{ TEXT("custom"), EMantleThumbnailMode::Custom } })
		{
			FMantleCosmeticDraft Draft;
			Draft.Class = EMantleCosmeticClass::Overlay;
			Draft.Slot = Args[2];
			Draft.Name = TEXT("Sample overlay");
			Draft.bPublic = false;
			Draft.OverlayMaskFile = Args[1];
			Draft.Thumbnail.Mode = Sample.Mode;
			Draft.Thumbnail.Angle = FRotator(-8.0, 150.0, 0.0);
			Draft.Thumbnail.ImageFile.FilePath = Args[3];
			const FMantleCosmeticPackage Package = FMantleCosmeticExport::Build(Draft, *Contract);
			const FMantleCosmeticUploadAsset* Picture = Package.ThumbnailKey.IsEmpty() ? nullptr : Package.FindAsset(Package.ThumbnailKey);
			const FString File = FPaths::Combine(Args[0], FString::Printf(TEXT("overlay_thumbnail_%s.png"), Sample.Name));
			const bool bSaved = Picture && FFileHelper::SaveArrayToFile(Picture->Bytes, *File);
			FString Note;
			for (const FString& N : Package.Notes)
			{
				if (N.StartsWith(TEXT("Overlay")))
				{
					Note = N;
				}
			}
			UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_SAMPLES %s %s errors=%d note=\"%s\""), *File, bSaved ? TEXT("saved") : TEXT("NOT saved"), Package.HasErrors() ? 1 : 0, *Note);
		}
		if (Args.Num() > 4 && Args[4].Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	/**
	 * #54: a placed picture through the panel's own export (Build): the picture put on the reference body or face at the default chest/face spot moved by
	 * (OffsetX, OffsetZ) cm, then the baked mask and the Automatic thumbnail (the avatar wearing it) as PNGs in Directory. Uploads nothing.
	 */
	void RunPlacedSample(const TArray<FString>& Args)
	{
		if (Args.Num() < 5)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_OVERLAY_PLACED usage: Mantle.Dev.OverlayPlacedSample <Directory> <Picture.png> <Slot> <OffsetX> <OffsetZ> [WidthCm] [DepthCm] [Angle=<pitch>/<yaw>] [Zoom=<z>] [Relief=<n>] [Exit]"));
			return;
		}
		IFileManager::Get().MakeDirectory(*Args[0], true);
		// Exit however this ends, so an unattended run never leaves an editor behind.
		ON_SCOPE_EXIT
		{
			if (Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("Exit"), ESearchCase::IgnoreCase); }))
			{
				FPlatformMisc::RequestExit(false);
			}
		};
		FString Error;
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&Error);
		if (!Contract)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_OVERLAY_PLACED failed: %s"), *Error);
			return;
		}
		FMantleCosmeticDraft Draft;
		Draft.Class = EMantleCosmeticClass::Overlay;
		Draft.Slot = Args[2];
		Draft.Name = TEXT("Placed sample");
		Draft.bPublic = false;
		Draft.bOverlayPlaced = true;
		Draft.OverlayPictureFile = Args[1];
		Draft.OverlayPlacement.WidthCm = Args.Num() > 5 && Args[5].IsNumeric() ? FCString::Atof(*Args[5]) : 14.f;
		if (Args.Num() > 6 && Args[6].IsNumeric())
		{
			Draft.OverlayPlacement.DepthCm = FCString::Atof(*Args[6]);
		}
		// Angle=<pitch>,<yaw> and Zoom=<z> choose the thumbnail's angle and closeness (a grazing close-up to read a scar's relief); else Automatic.
		Draft.Thumbnail.Mode = EMantleThumbnailMode::Automatic;
		for (const FString& Arg : Args)
		{
			FString Pitch, Yaw;
			if (Arg.StartsWith(TEXT("Angle=")) && Arg.Mid(6).Split(TEXT("/"), &Pitch, &Yaw))
			{
				Draft.Thumbnail.Mode = EMantleThumbnailMode::Angle;
				Draft.Thumbnail.Angle = FRotator(FCString::Atof(*Pitch), FCString::Atof(*Yaw), 0.0);
			}
			else if (Arg.StartsWith(TEXT("Zoom=")))
			{
				Draft.Thumbnail.Zoom = FCString::Atof(*Arg.Mid(5));
			}
			else if (Arg.StartsWith(TEXT("Relief=")))
			{
				Draft.OverlayPlacement.Relief = FCString::Atof(*Arg.Mid(7));
			}
		}
		// The same default spot the panel starts on, moved by the offset.
		USkeletalMesh* Mesh = FMantleOverlayBake::DefaultMesh(FMantleOverlayChecks::IsFaceSlot(Draft.Slot), Contract);
		TArray<FMantleOverlayBake::FTriangle> Triangles;
		FTransform Spot;
		if (!Mesh || !FMantleOverlayBake::ReadMesh(*Mesh, Triangles, Error)
			|| !MantleOverlayPlaceMath::DefaultPlacement(Triangles, Mesh->GetBounds().GetBox(), FVector(FCString::Atof(*Args[3]), 0.0, FCString::Atof(*Args[4])), Spot))
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_OVERLAY_PLACED no spot on the skin: %s"), *Error);
			if (Mesh)
			{
				// Where the skin is along the row the offset asked for, so the next try can aim.
				FBox Skin(ForceInit);
				for (const FMantleOverlayBake::FTriangle& Triangle : Triangles)
				{
					for (const FVector3f& Corner : Triangle.Position) { Skin += FVector(Corner); }
				}
				UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED skin triangles span %s"), *Skin.ToString());
				if (const FSkeletalMeshModel* Imported = Mesh->GetImportedModel(); Imported && Imported->LODModels.IsValidIndex(0))
				{
					for (const FSkelMeshSection& Section : Imported->LODModels[0].Sections)
					{
						UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED section slot=%s triangles=%u disabled=%d"),
							Mesh->GetMaterials().IsValidIndex(Section.MaterialIndex) ? *Mesh->GetMaterials()[Section.MaterialIndex].MaterialSlotName.ToString() : TEXT("?"),
							Section.NumTriangles, Section.bDisabled ? 1 : 0);
					}
				}
				const FBox Box = Mesh->GetBounds().GetBox();
				UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED mesh=%s triangles=%d bounds=%s"), *Mesh->GetName(), Triangles.Num(), *Box.ToString());
				const double Z = Box.Min.Z + Box.GetSize().Z * 0.72 + FCString::Atof(*Args[4]);
				for (double X = -60.0; X <= 60.0; X += 4.0)
				{
					FVector Point, Normal;
					if (MantleOverlayPlaceMath::RayOnTriangles(Triangles, FVector(X, 500.0, Z), FVector(0.0, -1.0, 0.0), Point, Normal))
					{
						UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED row z=%.0f x=%.0f hit y=%.1f normal=(%.2f,%.2f,%.2f)"), Z, X, Point.Y, Normal.X, Normal.Y, Normal.Z);
					}
				}
			}
			return;
		}
		Draft.OverlayPlacement.Transform = Spot;
		const FMantleCosmeticPackage Package = FMantleCosmeticExport::Build(Draft, *Contract);
		for (const FMantleCosmeticIssue& Issue : Package.Issues)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED issue: %s"), *Issue.Message);
		}
		const auto Save = [&Args](const FMantleCosmeticPackage& From, const FString& Key, const TCHAR* Name)
		{
			const FMantleCosmeticUploadAsset* Asset = Key.IsEmpty() ? nullptr : From.FindAsset(Key);
			const FString File = FPaths::Combine(Args[0], Name);
			const bool bSaved = Asset && FFileHelper::SaveArrayToFile(Asset->Bytes, *File);
			UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED %s %s"), *File, bSaved ? TEXT("saved") : TEXT("NOT saved"));
		};
		Save(Package, Package.MaskKey, TEXT("placed_mask.png"));
		Save(Package, Package.OverlayNormalKey, TEXT("placed_normal.png"));
		Save(Package, Package.ThumbnailKey, TEXT("placed_thumbnail.png"));
		UE_LOG(LogMantle, Display, TEXT("MANTLE_OVERLAY_PLACED done errors=%d"), Package.HasErrors() ? 1 : 0);
	}

	FAutoConsoleCommand OverlayPlacedCommand(
		TEXT("Mantle.Dev.OverlayPlacedSample"),
		TEXT("Mantle.Dev.OverlayPlacedSample <Directory> <Picture.png> <Slot> <OffsetX> <OffsetZ> [WidthCm] [DepthCm] [Angle=<pitch>/<yaw>] [Zoom=<z>] [Relief=<n>] [Exit]: a placed picture through the panel's export; the baked mask, normal map and Automatic thumbnail as PNGs. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunPlacedSample));

	FAutoConsoleCommand OverlaySamplesCommand(
		TEXT("Mantle.Dev.OverlayThumbnailSamples"),
		TEXT("Mantle.Dev.OverlayThumbnailSamples <Directory> <Mask.png> <Slot> <Custom.png> [Exit]: an overlay's thumbnail in each choice through the panel's export, one PNG each. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunOverlaySamples));
}
