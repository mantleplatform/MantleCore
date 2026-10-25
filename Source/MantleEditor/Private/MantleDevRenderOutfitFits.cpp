// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderOutfitFits <Outfit> <Directory> [female|male|auto] [slot] [size] [anim=<AnimSequence>...]
//   [on=<MetaHuman Character> onbody=<its converted body> onface=<its converted face>]
// With on=, the Outfit is fitted onto that one converted MetaHuman (as the panel's added fit bodies are) instead of the standard
// bodies, rendered as <Outfit>_on_<Character>_*.png.
// Fits an Outfit onto the standard bodies exactly as CheckCosmetic does (FMantleEditorActions::MakeCosmeticDraft, then
// FMantleParametricOutfit::Prepare per fit body) and renders each fit as the catalog thumbnail does (FMantleCosmeticThumbnail,
// GarmentSubject: the engine's thumbnail scene, reference pose, framed on the garment, on that size's own body and face), from
// the front, a quarter turn, the side and the back, into <Directory>/<Outfit>_<size id>_{front,quarter,side,back}.png. Nothing is saved or uploaded. Logs
// MANTLE_RENDER_FITS lines.
// Each anim= (a UEFN mannequin clip, such as M_Neutral_Stand_Idle_Loop) also renders every fit moving as players see it
// (FMantleEmoteCheck::GarmentPoser), one sheet per fit and clip, <Outfit>_<size id>_<clip>.png: two moments, front and side, the
// garment as fitted on the top row and, for one skinned to root alone, with the body's skin the upload gives it below
// (FMantleGarmentOutfit::FollowsRootOnly, TransferWeights).
// With an accessory slot it makes the rigid piece as CheckCosmetic does (FMantleParametricOutfit::PrepareRigid) and renders
// the catalog thumbnail itself (AccessorySubject, <Outfit>_<slot>_thumbnail.png, and from behind, _thumbnail_back.png) and the piece worn on the body it was made on,
// hung from that body's slot socket as an avatar wears it (<Outfit>_<slot>_worn_{front,back}.png).
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEditorActions.h"
#include "MantleEmoteCheck.h"
#include "MantleGarmentOutfit.h"
#include "MantleParametricOutfit.h"
#include "MantlePng.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MetaHumanCharacter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRenderOutfitFits, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevRenderOutfitFitsPrivate
{
	/** A garment fit's views: name and turn from the front (degrees). */
	const TArray<TPair<const TCHAR*, float>>& FitViews()
	{
		static const TArray<TPair<const TCHAR*, float>> Views = { { TEXT("front"), 0.0f }, { TEXT("quarter"), 35.0f }, { TEXT("side"), 90.0f }, { TEXT("back"), 180.0f } };
		return Views;
	}

	bool Save(const FMantleThumbnailSubject& Shot, int32 Size, const FString& File, FString& OutError)
	{
		TArray<FColor> Pixels;
		return FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, OutError) && FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
	}

	/**
	 * Garment with its authoring body's skin when it is skinned to root alone, as the upload gives it (MantleCosmeticExport's
	 * garment export); null when it follows the body already. OutNote says which.
	 */
	USkeletalMesh* WithBodySkin(USkeletalMesh* Garment, USkeletalMesh* Body, const FMantleAvatarContract& Contract, TArray<TStrongObjectPtr<UObject>>& KeepAlive,
		FString& OutNote)
	{
		FMantleGlbMesh Mesh, Skin;
		TArray<UMaterialInterface*> Materials, SkinMaterials;
		int32 Triangles = 0;
		FString Error;
		if (!FMantleCosmeticExport::ReadSkeletalMesh(Garment, 0, Mesh, Materials, Triangles, Error))
		{
			OutNote = FString::Printf(TEXT("cannot be read (%s)"), *Error);
			return nullptr;
		}
		if (!FMantleGarmentOutfit::FollowsRootOnly(Mesh))
		{
			OutNote = FString::Printf(TEXT("follows the body already (%d bones)"), Mesh.Joints.Num());
			return nullptr;
		}
		int32 Far = 0;
		double MeanCm = 0.0;
		USkeletalMesh* Fixed = FMantleCosmeticExport::ReadSkeletalMesh(Body, FMantleCosmeticExport::FindReferenceLOD(Body, Contract), Skin, SkinMaterials, Triangles, Error)
			&& FMantleGarmentOutfit::TransferWeights(Mesh, Skin, Far, MeanCm, Error) ? FMantleParametricOutfit::MakeMesh(Mesh, Materials, KeepAlive, Error) : nullptr;
		OutNote = Fixed ? FString::Printf(TEXT("skinned to root alone; took the body's skin (%d bones, mean %.2f cm from it)"), Mesh.Joints.Num(), MeanCm)
			: FString::Printf(TEXT("skinned to root alone, and the body's skin could not be taken (%s)"), *Error);
		return Fixed;
	}

	/** One fit moving: Clip's sheet of the garment as fitted, and below it Fixed (when set), each at two moments, front and side. */
	void RenderMoving(UAnimSequence* Clip, USkeletalMesh* Garment, USkeletalMesh* Fixed, const FMantleOutfitAuthoringTarget& Target, const FString& File, int32 Size)
	{
		TFunction<void(FMantleThumbnailSubject&, float)> PoseAt;
		float Duration = 0.0f;
		FString Error;
		if (!FMantleEmoteCheck::GarmentPoser(Clip, Target.Body.Get(), PoseAt, Duration, Error))
		{
			UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS %s %s FAILED: %s"), *Target.Id, *Clip->GetName(), *Error);
			return;
		}
		const TArray<float> Times = { Duration * 0.25f, Duration * 0.75f };
		TArray<TArray<FColor>> Tiles;
		for (USkeletalMesh* Shown : { Garment, Fixed })
		{
			if (!Shown)
			{
				continue;
			}
			for (const float Time : Times)
			{
				for (const float Turn : { 0.0f, 90.0f })
				{
					FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::GarmentSubject(Shown, Target.Body.Get(), Target.Face.Get());
					PoseAt(Shot, Time);
					Shot.View.Yaw += Turn;
					TArray<FColor>& Pixels = Tiles.AddDefaulted_GetRef();
					if (!FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error))
					{
						UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS %s %s FAILED at %.2f s: %s"), *Target.Id, *Clip->GetName(), Time, *Error);
						return;
					}
				}
			}
		}
		int32 Width = 0, Height = 0;
		const TArray<FColor> Sheet = FMantleEmoteCheck::Tile(Tiles, Size, 4, Width, Height);
		const bool bWritten = FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Width, Height, Sheet), *File);
		UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s %s %s (%.2f s and %.2f s, front and side; %s)"), *Target.Id, *Clip->GetName(),
			bWritten ? *File : TEXT("FAILED"), Times[0], Times[1], Fixed ? TEXT("top as fitted, below with the body's skin") : TEXT("as fitted"));
	}

	void RunAccessory(UObject* Outfit, const FMantleAvatarContract& Contract, const FString& Slot, EMantleFitSex Sex, const FString& Directory, int32 Size)
	{
		const FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Accessory, Outfit, Slot, FString(), Contract, Sex);
		FMantleCosmeticDraft Prepared;
		TArray<TStrongObjectPtr<UObject>> KeepAlive;
		FString Error, Note;
		FMantleParametricOutfit::FRigidPivot Pivot;
		UStaticMesh* Rigid = FMantleParametricOutfit::PrepareRigid(Draft, Contract, Prepared, KeepAlive, Error, &Note)
			&& FMantleParametricOutfit::RigidPivot(Contract, Slot, Pivot, Error) ? Cast<UStaticMesh>(Prepared.Source.Get()) : nullptr;
		if (!Rigid)
		{
			UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS %s FAILED: %s"), *Slot, *Error);
			return;
		}
		UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s rigid bounds=%s: %s"), *Slot, *Rigid->GetBoundingBox().ToString(), *Note);
		for (const FStaticMaterial& Material : Rigid->GetStaticMaterials())
		{
			UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s material %s two-sided=%d"), *Slot,
				Material.MaterialInterface ? *Material.MaterialInterface->GetPathName() : TEXT("none"), Material.MaterialInterface && Material.MaterialInterface->IsTwoSided() ? 1 : 0);
		}
		// The catalog thumbnail, as the upload renders it, and the same from behind.
		for (const TCHAR* View : { TEXT("thumbnail"), TEXT("thumbnail_back") })
		{
			FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::AccessorySubject(Rigid, Prepared.AccessoryWorn, Prepared.bAccessoryBackFaces);
			if (FCString::Strcmp(View, TEXT("thumbnail_back")) == 0) { Shot.View.Yaw += 180.0f; }
			const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s_%s.png"), *Outfit->GetName(), *Slot, View));
			const bool bWritten = Save(Shot, Size, File, Error);
			UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s %s %s %s"), *Slot, View, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}

		// Worn: the size's body parts in their reference pose, the piece on the slot socket of the first that has its bone.
		TArray<FMantleOutfitSourceSize> Sizes;
		FMantleParametricOutfit::ListSizes(Outfit, Sizes, Error);
		const FMantleOutfitSourceSize* Source = Sizes.FindByPredicate([&Prepared](const FMantleOutfitSourceSize& Entry) { return Entry.Name == Prepared.OutfitSourceSize; });
		TArray<USkeletalMesh*> Parts;
		TOptional<FTransform> Socket;
		for (const FSoftObjectPath& Path : Source ? Source->BodyParts : TArray<FSoftObjectPath>())
		{
			USkeletalMesh* Part = Cast<USkeletalMesh>(Path.TryLoad());
			if (!Part) { continue; }
			Parts.Add(Part);
			const FReferenceSkeleton& Skeleton = Part->GetRefSkeleton();
			const int32 Bone = Skeleton.FindRawBoneIndex(FName(*Pivot.Bone));
			if (Socket.IsSet() || Bone == INDEX_NONE) { continue; }
			FTransform Pose = FTransform::Identity;
			for (int32 Index = Bone; Index != INDEX_NONE; Index = Skeleton.GetRawParentIndex(Index)) { Pose = Pose * Skeleton.GetRawRefBonePose()[Index]; }
			// The body's own socket on that bone, when it carries one (its offset is what an avatar wears it with).
			const USkeletalMeshSocket* Own = Part->FindSocket(FName(*Slot));
			Socket = Own && Own->BoneName == FName(*Pivot.Bone) ? Own->GetSocketLocalTransform() * Pose : Pose;
		}
		if (Parts.IsEmpty() || !Socket.IsSet())
		{
			UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS %s worn FAILED: no body part of size %s has a %s bone"), *Slot, *Prepared.OutfitSourceSize, *Pivot.Bone);
			return;
		}
		const FBox Worn = Rigid->GetBoundingBox().TransformBy(Socket.GetValue());
		FMantleThumbnailSubject Subject = FMantleCosmeticThumbnail::GarmentSubject(Parts[0], Parts.IsValidIndex(1) ? Parts[1] : nullptr,
			Parts.IsValidIndex(2) ? Parts[2] : nullptr, nullptr, Worn);
		Subject.StaticMesh = Rigid;
		Subject.StaticTransform = Socket.GetValue();
		if (Prepared.bAccessoryBackFaces) { Subject.StaticBackFaces.Reset(FMantleCosmeticThumbnail::BackFaces(Rigid)); }
		for (const TCHAR* View : { TEXT("front"), TEXT("back") })
		{
			FMantleThumbnailSubject Shot = Subject;
			if (FCString::Strcmp(View, TEXT("back")) == 0) { Shot.View.Yaw += 180.0f; }
			const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s_worn_%s.png"), *Outfit->GetName(), *Slot, View));
			const bool bWritten = Save(Shot, Size, File, Error);
			UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s worn %s %s %s"), *Slot, View, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
	}

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS usage: Mantle.Dev.RenderOutfitFits <Outfit> <Directory> [female|male|auto] [slot] [size]"));
			return;
		}
		FString Path = Args[0];
		if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		UObject* Outfit = LoadObject<UObject>(nullptr, *Path);
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		EMantleFitSex Sex = EMantleFitSex::Auto;
		FString WhyNot;
		if (!Outfit || !Contract || !(FMantleParametricOutfit::IsSource(Outfit) || FMantleParametricOutfit::IsGarmentSource(Outfit)) || (Args.Num() > 2 && !FMantleEditorActions::ParseFitSex(Args[2], Sex))
			|| !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS failed: %s is not an Outfit, there is no contract, the sex is wrong, or nothing can render (%s)"), *Path, *WhyNot);
			return;
		}
		const FString Directory = Args[1];
		const FString Slot = Args.Num() > 3 ? Args[3] : FString(TEXT("top"));
		const int32 Size = Args.Num() > 4 ? FMath::Clamp(FCString::Atoi(*Args[4]), 128, 2048) : 1024;
		TArray<UAnimSequence*> Clips;
		// on=<Character> onbody=<Body> onface=<Face>: fit onto that converted MetaHuman (as the panel's "Add fit body" does)
		// instead of the standard bodies.
		FMantleOutfitAuthoringTarget On;
		for (int32 Index = 5; Index < Args.Num(); ++Index)
		{
			const FString& Arg = Args[Index];
			const bool bCharacter = Arg.StartsWith(TEXT("on="));
			const bool bBody = Arg.StartsWith(TEXT("onbody="));
			const bool bFace = Arg.StartsWith(TEXT("onface="));
			if (!bCharacter && !bBody && !bFace)
			{
				continue;
			}
			FString OnPath = Arg.RightChop(bCharacter ? 3 : 7);
			if (!FPackageName::GetShortName(OnPath).Contains(TEXT(".")))
			{
				OnPath += TEXT(".") + FPackageName::GetShortName(OnPath);
			}
			if (bCharacter)
			{
				On.Character = LoadObject<UMetaHumanCharacter>(nullptr, *OnPath);
				On.Id = TEXT("on_") + FPackageName::ObjectPathToObjectName(OnPath);
			}
			else
			{
				(bBody ? On.Body : On.Face) = LoadObject<USkeletalMesh>(nullptr, *OnPath);
			}
		}
		if (!On.Id.IsEmpty() && (!On.Character.IsValid() || !On.Body.IsValid() || !On.Face.IsValid()))
		{
			UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS failed: on= needs a MetaHuman Creator character with onbody= and onface= (its converted body and face)"));
			return;
		}
		for (int32 Index = 5; Index < Args.Num(); ++Index)
		{
			FString ClipPath;
			if (!Args[Index].Split(TEXT("anim="), nullptr, &ClipPath) || ClipPath.IsEmpty())
			{
				continue;
			}
			if (!FPackageName::GetShortName(ClipPath).Contains(TEXT(".")))
			{
				ClipPath += TEXT(".") + FPackageName::GetShortName(ClipPath);
			}
			UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *ClipPath);
			if (!Clip)
			{
				UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS failed: %s is not an animation sequence"), *ClipPath);
				return;
			}
			Clips.Add(Clip);
		}
		const FMantleContractSlot* SlotEntry = Contract->FindSlot(Slot);
		if (SlotEntry && SlotEntry->ItemClass == TEXT("rigid_accessory"))
		{
			RunAccessory(Outfit, *Contract, Slot, Sex, Directory, Size);
			UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS done"));
			return;
		}
		FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Garment, Outfit, Slot, FString(), *Contract, Sex);
		// A skeletal-mesh or cloth-asset garment is wrapped as an Outfit first, as CheckCosmetic does (BuildCore).
		TArray<TStrongObjectPtr<UObject>> WrapKeepAlive;
		TSharedPtr<void> WrapLease;
		if (FMantleParametricOutfit::IsGarmentSource(Outfit))
		{
			FMantleCosmeticDraft Wrapped;
			FString Why;
			if (!FMantleParametricOutfit::PrepareWrapped(Draft, *Contract, Wrapped, WrapKeepAlive, WrapLease, Why))
			{
				UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS %s FAILED: it cannot be made in sizes: %s"), *Outfit->GetPathName(), *Why);
				return;
			}
			Draft = MoveTemp(Wrapped);
			// Made with Make Outfit from garment: the stages before the resize too, the garment on its own character's body and the
			// Outfit's authored size on the MetaHuman made from that body (<Garment>_original_*, <Garment>_twin_*).
			UChaosOutfitAsset* Made = Cast<UChaosOutfitAsset>(Draft.Source.Get());
			USkeletalMesh* Body = FMantleParametricOutfit::FindBodyBeside(Cast<USkeletalMesh>(Outfit));
			TArray<FMantleOutfitSourceSize> Sizes;
			FString SizesError;
			USkeletalMesh* Twin = Made && FMantleParametricOutfit::ListSizes(Made, Sizes, SizesError) && Sizes[0].BodyParts.Num() > 0
				? Cast<USkeletalMesh>(Sizes[0].BodyParts[0].TryLoad()) : nullptr;
			USkeletalMesh* Authored = NewObject<USkeletalMesh>();
			WrapKeepAlive.Emplace(Authored);
			FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ChaosClothAssetTools"));
			if (Made && Twin && Made->ExportToSkeletalMesh(*Authored))
			{
				USkinnedAsset* Compiling[] = { Authored };
				FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
				if (!Authored->GetSkeleton())
				{
					USkeleton* Skeleton = NewObject<USkeleton>();
					WrapKeepAlive.Emplace(Skeleton);
					Skeleton->MergeAllBonesToBoneTree(Authored, false);
					Authored->SetSkeleton(Skeleton);
				}
			}
			else
			{
				Authored = nullptr;
			}
			const bool bMade = Body && Authored && Made->GetPackage()->GetMetaData().FindValue(Made, FMantleGarmentOutfit::MadeFromGarmentKey);
			for (const TCHAR* Stage : { TEXT("original"), TEXT("twin") })
			{
				if (!bMade)
				{
					break;
				}
				const bool bOriginal = FCString::Strcmp(Stage, TEXT("original")) == 0;
				const FMantleThumbnailSubject Subject = bOriginal ? FMantleCosmeticThumbnail::GarmentSubject(Cast<USkeletalMesh>(Outfit), Body, nullptr)
					: FMantleCosmeticThumbnail::GarmentSubject(Authored, Twin, nullptr);
				for (const TPair<const TCHAR*, float>& FitView : FitViews())
				{
					const TCHAR* View = FitView.Key;
					FMantleThumbnailSubject Shot = Subject;
					Shot.View.Yaw += FitView.Value;
					const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s_%s.png"), *Outfit->GetName(), Stage, View));
					const bool bWritten = Save(Shot, Size, File, Why);
					UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s %s %s %s"), Stage, View, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Why);
				}
			}
		}
		if (!On.Id.IsEmpty())
		{
			Draft.OutfitTargets = { On };
		}
		UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s: %d fit bodies (%s)"), *Outfit->GetPathName(), Draft.OutfitTargets.Num(), *Draft.FitBodiesStatus);
		for (const FMantleOutfitAuthoringTarget& Target : Draft.OutfitTargets)
		{
			// The fit CheckCosmetic makes for this body (FMantleCosmeticExport::Build's per-source draft).
			FMantleCosmeticDraft Input = Draft;
			Input.OutfitTargets.Reset();
			Input.OutfitSourceSize.Reset();
			Input.OutfitAuthoringCharacter = Target.Character;
			Input.bOutfitAuthoringInMemory = Target.bInMemory;
			Input.AuthoringBody = Target.Body;
			Input.AuthoringFace = Target.Face;
			FMantleCosmeticDraft Prepared;
			TArray<TStrongObjectPtr<UObject>> KeepAlive;
			FString Error;
			USkeletalMesh* Garment = FMantleParametricOutfit::Prepare(Input, *Contract, Prepared, KeepAlive, Error) ? Cast<USkeletalMesh>(Prepared.Source.Get()) : nullptr;
			if (!Garment)
			{
				UE_LOG(LogMantleRenderOutfitFits, Error, TEXT("MANTLE_RENDER_FITS %s FAILED: %s"), *Target.Id, *Error);
				continue;
			}
			// The catalog thumbnail's subject: the garment on this size's own body and face.
			FMantleThumbnailSubject Subject = FMantleCosmeticThumbnail::GarmentSubject(Garment, Target.Body.Get(), Target.Face.Get());
			for (const TPair<const TCHAR*, float>& FitView : FitViews())
			{
				const TCHAR* View = FitView.Key;
				FMantleThumbnailSubject Shot = Subject;
				Shot.View.Yaw += FitView.Value;
				TArray<FColor> Pixels;
				const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s_%s.png"), *Outfit->GetName(), *Target.Id, View));
				const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
					&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
				UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s %s %s %s"), *Target.Id, View, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
			}
			if (Clips.IsEmpty())
			{
				continue;
			}
			FString SkinNote;
			USkeletalMesh* Fixed = WithBodySkin(Garment, Target.Body.Get(), *Contract, KeepAlive, SkinNote);
			UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS %s %s"), *Target.Id, *SkinNote);
			for (UAnimSequence* Clip : Clips)
			{
				RenderMoving(Clip, Garment, Fixed, Target, FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s_%s.png"), *Outfit->GetName(), *Target.Id, *Clip->GetName())),
					FMath::Min(Size, 512));
			}
		}
		UE_LOG(LogMantleRenderOutfitFits, Display, TEXT("MANTLE_RENDER_FITS done"));
	}

	FAutoConsoleCommand GMantleDevRenderOutfitFitsCommand(
		TEXT("Mantle.Dev.RenderOutfitFits"),
		TEXT("Mantle.Dev.RenderOutfitFits <Outfit> <Directory> [female|male|auto] [slot] [size] [anim=<AnimSequence>...] -- fits the Outfit onto the standard bodies as CheckCosmetic does and renders ")
		TEXT("each as the catalog thumbnail does, front and back, into Directory (and moving through each anim=, as fitted and with the body's skin); with an accessory slot, its catalog thumbnail and the piece worn on its body. Saves and uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
