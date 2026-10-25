// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.SectionOutfits <Body> <Directory> [TurnOff keys, comma-separated]
// #55 step 2 end to end, as the Cosmetics panel runs it: Convert <Body> (the bare-skin line confirmed, the Triangles, Atlas and Bake fixes
// off unless TurnOff says otherwise), then the clothing in the body's own material sections is cut into pieces
// (FMantleGarmentOutfit::MakeSectionMeshes) and made into MetaHuman Outfits on the MetaHuman Convert made (MakeFromWork); each Outfit is
// then drawn as the catalog thumbnail does (Mantle.Dev.RenderOutfitFits) into <Directory>. Before that, for telling where a piece goes
// wrong: the character as it came (source_*), each cut piece alone and on the bare body Strip left (piece_<slot>_alone_*, _on_body_*),
// and each Outfit at the size it was made on the MetaHuman it was made on (made_<slot>_*), front, side and back. Needs a renderer.
// Mantle.Dev.GarmentOutfits <Body> <Directory> <Garment,...|found|rows> [draw-on:<Mesh>] [draw-only] does the same for clothing in its own
// meshes beside a bare body (#102); rows takes the clothing the pack review's rows give for <Body> (#55), draw-only stops after the pictures. Saves what Convert and the
// Outfits write under <body folder>/Converted; uploads nothing. Logs MANTLE_SECTION_OUTFITS lines.
#include "MantleAvatarConverter.h"
#include "MantleCharacterSplit.h"
#include "MantleCosmeticThumbnail.h"
#include "MantlePng.h"
#include "MantleAvatarContract.h"
#include "MantleGarmentOutfit.h"
#include "MantleMetaHumanPose.h"

#include "Animation/Skeleton.h"
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleSectionOutfits, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevSectionOutfitsPrivate
{
	/** Mesh on Body (none: alone) as the catalog thumbnail draws it, front, side and back, into <Directory>/<Name>_<view>.png. */
	void Draw(USkeletalMesh* Mesh, USkeletalMesh* Body, const FString& Directory, const FString& Name, TConstArrayView<USkeletalMesh*> With = {})
	{
		FMantleThumbnailSubject Subject = FMantleCosmeticThumbnail::GarmentSubject(Mesh, Body, nullptr);
		// More pieces worn with it (its trims, the rest of the outfit), drawn in the same scene.
		for (USkeletalMesh* Other : With)
		{
			if (Other)
			{
				Subject.SkeletalMeshes.Add(Other);
				if (Subject.HiddenSlots.Num() > 0) { Subject.HiddenSlots.AddDefaulted(); }
				if (Subject.MaterialOverrides.Num() > 0) { Subject.MaterialOverrides.AddDefaulted(); }
			}
		}
		for (const TPair<const TCHAR*, float> View : { TPair<const TCHAR*, float>(TEXT("front"), -90.0f), TPair<const TCHAR*, float>(TEXT("side"), 0.0f), TPair<const TCHAR*, float>(TEXT("back"), 90.0f) })
		{
			Subject.View = FRotator(-4.0f, View.Value, 0.0f);
			TArray<FColor> Pixels;
			FString CaptureError;
			const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s.png"), *Name, View.Key));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Subject, 1024, Pixels, CaptureError) && FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(1024, 1024, Pixels), *File);
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS picture %s %s"), *File, bWritten ? TEXT("") : *CaptureError);
		}
	}

	/** Piece's Outfit at the size it was made, on the MetaHuman it was made on (before any resize onto Epic's bodies): made_<Name>_<view>.png. */
	void DrawMade(const FMantleGarmentOutfitPiece& Piece, const FString& AuthoringBody, const FString& Directory, const FString& Name)
	{
		USkeletalMesh* Authoring = LoadObject<USkeletalMesh>(nullptr, *AuthoringBody, nullptr, LOAD_NoWarn | LOAD_Quiet);
		UChaosOutfitAsset* Outfit = !Piece.Outfit.IsEmpty() ? LoadObject<UChaosOutfitAsset>(nullptr, *Piece.Outfit, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
		if (!Authoring || !Outfit)
		{
			return;
		}
		FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ChaosClothAssetTools"));
		TStrongObjectPtr<USkeletalMesh> Exported(NewObject<USkeletalMesh>());
		TStrongObjectPtr<USkeleton> Skeleton;
		if (!Outfit->ExportToSkeletalMesh(*Exported.Get()))
		{
			return;
		}
		USkinnedAsset* Compiling[] = { Exported.Get() };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		if (!Exported->GetSkeleton())
		{
			Skeleton.Reset(NewObject<USkeleton>());
			Skeleton->MergeAllBonesToBoneTree(Exported.Get(), false);
			Exported->SetSkeleton(Skeleton.Get());
		}
		Draw(Exported.Get(), Authoring, Directory, FString::Printf(TEXT("made_%s"), *Name));
	}

	/**
	 * Mantle.Dev.GarmentOutfits <Body|bare-of:Mesh> <Directory> <Garment,Garment,...|found>: the separate-mesh route (#102, "Make Outfit from garment")
	 * for Body and its garments, drawn as SectionOutfits draws its pieces (piece_<garment>_on_body_*, made_<garment>_*), then as the
	 * catalog thumbnail does (Mantle.Dev.RenderOutfitFits). Logs MANTLE_SECTION_OUTFITS lines.
	 */
	void RunGarments(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantleSectionOutfits, Error, TEXT("MANTLE_SECTION_OUTFITS usage: Mantle.Dev.GarmentOutfits <Body> <Directory> <Garment,Garment,...>"));
			return;
		}
		// "bare-of:<Mesh>": the bare body the pack has beside that character (FMantleGarmentOutfit::FindBareBody).
		const bool bBareOf = Args[0].StartsWith(TEXT("bare-of:"));
		USkeletalMesh* Body = LoadObject<USkeletalMesh>(nullptr, bBareOf ? *Args[0].RightChop(8) : *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
		// "rows": the clothing the pack review's rows take for the mesh named (bare-of: the character), on the body they chose (#55).
		USkeletalMesh* Picked = Body;
		TOptional<FMantlePackReview> Review;
		if (Body && Args[2] == TEXT("rows"))
		{
			Review = FMantleCharacterSplit::ReviewFor(*Picked);
			const int32 Chosen = Review->BodyMesh();
			Body = Review->Meshes.IsValidIndex(Chosen) && Review->Meshes[Chosen].IsValid() ? Review->Meshes[Chosen].Get() : Body;
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS rows of %s: body %s"), *Picked->GetName(), *Body->GetName());
		}
		else if (Body && bBareOf)
		{
			FString Why;
			Body = FMantleGarmentOutfit::FindBareBody(*Body, Why);
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS bare body: %s"), Body ? *Why : TEXT("none in the pack"));
		}
		TArray<FString> Paths;
		Args[2].ParseIntoArray(Paths, TEXT(","), true);
		TArray<USkeletalMesh*> Garments;
		TArray<TArray<USkeletalMesh*>> Trims;
		// "found": the clothing Convert finds beside the body, its trims paired, as the Cosmetics panel offers it.
		if (Body && (Args[2] == TEXT("found") || Review.IsSet()))
		{
			TArray<FMantleConvertGarment> Found;
			TArray<FString> NotOffered, Paired;
			if (Review.IsSet())
			{
				FMantleGarmentOutfit::GarmentsFromReview(*Body, *Review, Found);
			}
			else
			{
				FMantleGarmentOutfit::FindGarments(*Body, Found, NotOffered, &Paired);
			}
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS paired: %s; not offered: %s"), *FString::Join(Paired, TEXT("; ")), *FString::Join(NotOffered, TEXT(", ")));
			for (const FMantleConvertGarment& Garment : Found)
			{
				UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS found %s slot=%s trims=%d why=%s"), *Garment.Name, *Garment.Slot, Garment.Trims.Num(), *Garment.Why);
				if (Garment.Why.IsEmpty() && Garment.Mesh.IsValid())
				{
					Garments.Add(Garment.Mesh.Get());
					TArray<USkeletalMesh*>& Its = Trims.AddDefaulted_GetRef();
					for (const TWeakObjectPtr<USkeletalMesh>& Trim : Garment.Trims)
					{
						Its.Add(Trim.Get());
					}
				}
			}
			Paths.Reset();
		}
		for (const FString& Path : Paths)
		{
			if (USkeletalMesh* Garment = LoadObject<USkeletalMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Garments.Add(Garment);
			}
			else
			{
				UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS %s is not a skeletal mesh; left out"), *Path);
			}
		}
		if (!Body || Garments.IsEmpty())
		{
			UE_LOG(LogMantleSectionOutfits, Error, TEXT("MANTLE_SECTION_OUTFITS failed: %s"), Body ? TEXT("no garment is a skeletal mesh") : TEXT("the body is not a skeletal mesh"));
			return;
		}
		// The pieces are drawn on a bare body as the creator made them: [draw-on:<Mesh>] (the pack's body with nothing on),
		// else the body itself (which may still wear underwear Convert takes off).
		// [draw-only]: the pictures of the pieces on the body, no Outfits made.
		USkeletalMesh* DrawOn = Body;
		bool bDrawOnly = false;
		for (int32 Arg = 3; Arg < Args.Num(); ++Arg)
		{
			if (Args[Arg].StartsWith(TEXT("draw-on:")))
			{
				if (USkeletalMesh* Bare = LoadObject<USkeletalMesh>(nullptr, *Args[Arg].RightChop(8), nullptr, LOAD_NoWarn | LOAD_Quiet))
				{
					DrawOn = Bare;
				}
			}
			bDrawOnly |= Args[Arg] == TEXT("draw-only");
		}
		Draw(DrawOn, nullptr, Args[1], TEXT("separated_body"));
		TArray<USkeletalMesh*> All;
		for (int32 Index = 0; Index < Garments.Num(); ++Index)
		{
			const TArray<USkeletalMesh*> Its = Trims.IsValidIndex(Index) ? Trims[Index] : TArray<USkeletalMesh*>();
			Draw(Garments[Index], DrawOn, Args[1], FString::Printf(TEXT("piece_%s_on_body"), *Garments[Index]->GetName()), Its);
			All.Append(Its);
		}
		// Everything offered (and its trims) worn together, to set beside the character's own picture.
		if (Garments.Num() > 1)
		{
			All.Append(TArrayView<USkeletalMesh*>(Garments).RightChop(1));
			Draw(Garments[0], DrawOn, Args[1], TEXT("all_on_body"), All);
		}
		if (bDrawOnly)
		{
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS done (draw-only)"));
			return;
		}
		FMantleGarmentOutfitResult Made;
		FString ConvertJson, Code, Refusal;
		const bool bMade = FMantleGarmentOutfit::Make(*Body, Garments, FString(), TEXT("Strip.0.BareSkin"), Made, ConvertJson, Code, Refusal, Trims);
		UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS Outfits on %s: %s (body gap %.2f cm)"), *Made.Character, bMade ? TEXT("made") : *Refusal, Made.BodyGapCm);
		for (int32 Index = 0; Index < Made.Pieces.Num() && Index < Garments.Num(); ++Index)
		{
			const FMantleGarmentOutfitPiece& Piece = Made.Pieces[Index];
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS %s: outfit=%s error=%s far=%d mean=%.2f cm heel=%.2f cm"), *Garments[Index]->GetName(),
				*Piece.Outfit, *Piece.Error, Piece.FarVertices, Piece.MeanSkinCm, Piece.HeelCm);
			DrawMade(Piece, Made.AuthoringBody, Args[1], Garments[Index]->GetName());
			if (!Piece.Outfit.IsEmpty() && GEngine)
			{
				GEngine->Exec(nullptr, *FString::Printf(TEXT("Mantle.Dev.RenderOutfitFits %s %s auto"), *Piece.Outfit, *Args[1]));
			}
		}
		UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS done"));
	}

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantleSectionOutfits, Error, TEXT("MANTLE_SECTION_OUTFITS usage: Mantle.Dev.SectionOutfits <Body> <Directory> [TurnOff]"));
			return;
		}
		FString ContractError;
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
		if (!Contract)
		{
			UE_LOG(LogMantleSectionOutfits, Error, TEXT("MANTLE_SECTION_OUTFITS failed: no contract (%s)"), *ContractError);
			return;
		}
		FMantleConvertOptions Options;
		FMantleConvertContext Context;
		FString Error;
		if (!FMantleAvatarConverter::MakeContextForPath(Args[0], Options, *Contract, Context, Error))
		{
			UE_LOG(LogMantleSectionOutfits, Error, TEXT("MANTLE_SECTION_OUTFITS failed: %s"), *Error);
			return;
		}
		for (const FString& Note : Context.Notes)
		{
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS convert note: %s"), *Note);
		}
		TArray<FString> Off;
		(Args.Num() > 2 ? Args[2] : FString(TEXT("Triangles.0.Mesh,Atlas.0.0,Bake.0.Mesh"))).ParseIntoArray(Off, TEXT(","), true);
		TArray<FMantleConvertFix> Fixes = FMantleAvatarConverter::Analyze(Context);
		const TArray<FString> Keys = FMantleAvatarConverter::FixKeys(Fixes);
		for (int32 Index = 0; Index < Fixes.Num(); ++Index)
		{
			if (Off.Contains(Keys[Index]))
			{
				Fixes[Index].bEnabled = false;
			}
			if (Keys[Index].EndsWith(TEXT(".BareSkin")))
			{
				Fixes[Index].bEnabled = true;
			}
		}
		// The pieces in the body's own sections even when the pack has its bare body and separate clothing (which Convert prefers):
		// this command is the merged-mesh route, for comparing the two.
		if (!Context.Garments.ContainsByPredicate([](const FMantleConvertGarment& Garment) { return Garment.Sections.Num() > 0; }) && Context.Parts[0].Original.IsValid())
		{
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS the pack's separate clothing would be used; cutting the sections anyway, for comparison"));
			FMantleGarmentOutfit::FindSectionGarments(*Context.Parts[0].Original.Get(), Context.Garments);
		}
		const FMantleConvertResult Result = FMantleAvatarConverter::Convert(Context, Fixes, Options, *Contract);
		UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS convert: %d applied, %d failed"), Result.Applied.Num(), Result.Failed.Num());
		for (const FMantleConvertOutcome& Failed : Result.Failed)
		{
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS convert failed %s: %s"), *Failed.Key, *Failed.Reason);
		}
		const FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(0);
		const FString Why = FMantleGarmentOutfit::WhyNoOutfits(Context, 0, /*bSectionPieces*/ true);
		if (!Work || !Why.IsEmpty())
		{
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS no Outfits: %s"), Work ? *Why : TEXT("Convert made no MetaHuman"));
			return;
		}
		const int32 Cut = FMantleGarmentOutfit::MakeSectionMeshes(Context);
		TArray<int32> Indices;
		TArray<USkeletalMesh*> Meshes;
		for (int32 Index = 0; Index < Context.Garments.Num(); ++Index)
		{
			const FMantleConvertGarment& Garment = Context.Garments[Index];
			if (Garment.Sections.Num() > 0 && Garment.Mesh.IsValid() && Garment.Error.IsEmpty())
			{
				Indices.Add(Index);
				Meshes.Add(Garment.Mesh.Get());
			}
			else if (Garment.Sections.Num() > 0)
			{
				UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS piece %s not cut: %s"), *Garment.Name, *Garment.Error);
			}
		}
		UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS cut %d pieces"), Cut);
		if (Context.Parts[0].Original.IsValid())
		{
			const TArray<FSkeletalMaterial>& Slots = Context.Parts[0].Original->GetMaterials();
			for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
			{
				UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS material slot %d: %s (%s)"), Slot, *Slots[Slot].MaterialSlotName.ToString(),
					Slots[Slot].MaterialInterface ? *Slots[Slot].MaterialInterface->GetName() : TEXT("none"));
			}
			Draw(Context.Parts[0].Original.Get(), nullptr, Args[1], TEXT("source"));
		}
		// Each cut piece as it is before the fit: alone, and on the bare body Strip left (the body it was made for). A piece
		// that is wrong here was cut wrong; one that is right here and wrong in the Outfit was changed by the fit.
		for (const int32 Index : Indices)
		{
			const FMantleConvertGarment& Garment = Context.Garments[Index];
			USkeletalMesh* PieceMesh = Garment.Mesh.Get();
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS piece %s: bounds %s (%d material slots)"), *Garment.Name, *PieceMesh->GetBounds().GetBox().ToString(), PieceMesh->GetMaterials().Num());
			Draw(PieceMesh, nullptr, Args[1], FString::Printf(TEXT("piece_%s_alone"), *Garment.Slot));
			Draw(PieceMesh, Work->SkinOnly.Get(), Args[1], FString::Printf(TEXT("piece_%s_on_body"), *Garment.Slot));
		}
		if (Meshes.Num() == 0)
		{
			return;
		}
		const FMantleConvertPart& Avatar = Context.Parts[0];
		const FString BodyName = Avatar.Original.IsValid() ? Avatar.Original->GetName() : Avatar.Name;
		const FString Folder = Context.OutputFolder / (FMantleMetaHumanSteps::AssetTag(BodyName) + TEXT("_Outfit"));
		FMantleGarmentOutfitResult Made;
		FString Code;
		FString Refusal;
		const bool bMade = FMantleGarmentOutfit::MakeFromWork(*Work, BodyName, Folder, Meshes, Made, Code, Refusal, /*bCutFromOwnMesh*/ true);
		UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS Outfits on %s: %s (body gap %.2f cm)"), *Made.Character, bMade ? TEXT("made") : *Refusal, Made.BodyGapCm);
		for (int32 Piece = 0; Piece < Indices.Num(); ++Piece)
		{
			const FMantleConvertGarment& Garment = Context.Garments[Indices[Piece]];
			const FMantleGarmentOutfitPiece* Result2 = bMade && Made.Pieces.IsValidIndex(Piece) ? &Made.Pieces[Piece] : nullptr;
			UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS %s (%s): outfit=%s error=%s far=%d mean=%.2f cm heel=%.2f cm"), *Garment.Name, *Garment.Slot,
				Result2 ? *Result2->Outfit : TEXT(""), Result2 ? *Result2->Error : *Refusal, Result2 ? Result2->FarVertices : 0, Result2 ? Result2->MeanSkinCm : 0.0, Result2 ? Result2->HeelCm : 0.0);
			if (Result2)
			{
				DrawMade(*Result2, Made.AuthoringBody, Args[1], Garment.Slot);
			}
			if (Result2 && !Result2->Outfit.IsEmpty() && GEngine)
			{
				GEngine->Exec(nullptr, *FString::Printf(TEXT("Mantle.Dev.RenderOutfitFits %s %s auto"), *Result2->Outfit, *Args[1]));
			}
		}
		UE_LOG(LogMantleSectionOutfits, Display, TEXT("MANTLE_SECTION_OUTFITS done"));
	}

	FAutoConsoleCommand GMantleDevSectionOutfitsCommand(
		TEXT("Mantle.Dev.SectionOutfits"),
		TEXT("Mantle.Dev.SectionOutfits <Body> <Directory> [TurnOff] -- Convert the body, cut the clothing in its own sections into pieces, make them MetaHuman Outfits and draw each as the catalog thumbnail does"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));

	FAutoConsoleCommand GMantleDevGarmentOutfitsCommand(
		TEXT("Mantle.Dev.GarmentOutfits"),
		TEXT("Mantle.Dev.GarmentOutfits <Body> <Directory> <Garment,Garment,...> -- make the garments beside a bare body MetaHuman Outfits and draw each stage as the catalog thumbnail does"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunGarments));
}
