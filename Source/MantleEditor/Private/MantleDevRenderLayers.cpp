// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderLayers <Directory> <Label> <body> <Garment>@<slot>[:tucked][:tallboots] ... [size=<pixels>]
// A local proof of a layered look (#65), no upload: two or more garments worn together on one of Epic's standard bodies, through
// the game's own layer steps. <body> is a standard body id (m_med_nrw, f_med_unw ...), made in memory (FMantleFitBodies).
// Each garment (an Outfit, a skeletal-mesh or cloth garment) is fitted onto that body exactly as CheckCosmetic does
// (FMantleEditorActions::MakeCosmeticDraft, FMantleParametricOutfit::PrepareWrapped for a non-Outfit, FMantleParametricOutfit::Prepare
// for that body's fit), and then the look is assembled as the client's dressed body does it (mantle client
// MantleAvatarGarments.cpp StartDressed): FMantleLayerFit::Apply over the garments from the skin outward (each pushed out of the ones
// under it; the covered parts of the ones under it cut and their edges sunk), then FMantleDressedBody::Compute on the body under the
// pushed garments, with each garment's skin mask (FMantleSkinMask::Derive on its source body, here the body it was fitted to, as the
// client derives it in FMantleCatalogGarmentFit, MantleCatalogGarmentFit.cpp:56; FMantleDressedBodyParams::SkinMaskDefaults, the
// client's default Mantle.Avatar.SkinMask 1). The garments are drawn in layer order (the merged look sorts by layer).
// The layer order is the contract's (limits.json garmentLayers.order from the skin outward, FMantleAvatarContract::ClothingRank),
// with its two switches: ":tucked" puts a top just under the bottom (tuck), ":tallboots" puts footwear just over it.
// Drawn as the catalog thumbnail draws a garment (FMantleCosmeticThumbnail: the dressed body, its face and every garment as shown,
// each with its own materials), from the front, a quarter turn, the side and the back, 30 frames after each camera move, into
// <Directory>/<Label>_{front,quarter,side,back}.png. Nothing is saved or uploaded. Logs MANTLE_RENDER_LAYERS lines (the layer and
// dressed-body counts, each picture, "done" or "failed").
#include "MantleAvatarContract.h"
#include "MantleBodyCoverage.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEditorActions.h"
#include "MantleFitBodies.h"
#include "MantleLayerFit.h"
#include "MantleParametricOutfit.h"
#include "MantlePng.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleSkinMask.h"
#include "MantleSkinnedMesh.h"

#include "Algo/StableSort.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRenderLayers, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevRenderLayersPrivate
{
	/** One garment of the look: where it came from, its slot and switches, and its fit on the body. */
	struct FWorn
	{
		FString Spec;
		FString Slot;
		bool bTucked = false;
		bool bTallBoots = false;
		int32 Rank = 0;
		TSharedPtr<FMantleSkinnedMesh> Fitted;
		TArray<FSkeletalMaterial> Materials;
		TUniquePtr<FMantleSkinMask> Mask;
	};

	/**
	 * The garment's place from the skin outward: the contract's order (limits.json garmentLayers.order), 100 apart, with the
	 * tuck (a tucked top just under the bottom) and tall boots (just over the bottom) between, and gloves under the sleeves of
	 * the top (limits.json gloves.alwaysUnder). The same order the client's FMantleAvatarContract::LayerRank encodes.
	 */
	int32 RankOf(const FMantleAvatarContract& Contract, const FWorn& Worn)
	{
		const int32 Bottom = Contract.ClothingRank(TEXT("bottom")) * 100;
		if (Worn.Slot == TEXT("top") && Worn.bTucked) return Bottom - 50;
		if (Worn.Slot == TEXT("footwear") && Worn.bTallBoots) return Bottom + 50;
		if (Worn.Slot == Contract.GlovesSlot) return Contract.ClothingRank(TEXT("top")) * 100 - 10;
		return Contract.ClothingRank(Worn.Slot) * 100;
	}

	/** Mesh with only the sections drawn with one of KeepSlots' material slots (every section when KeepSlots is unset). */
	void KeepSections(FMantleSkinnedMesh& Mesh, const TOptional<TArray<int32>>& KeepSlots)
	{
		if (!KeepSlots.IsSet()) return;
		for (FMantleSkinnedMeshLOD& LOD : Mesh.LODs)
		{
			TArray<uint32> Indices;
			TArray<FMantleSkinnedMeshSection> Sections;
			for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
			{
				if (!KeepSlots.GetValue().Contains(Section.MaterialIndex)) continue;
				FMantleSkinnedMeshSection Kept = Section;
				Kept.FirstIndex = Indices.Num();
				Indices.Append(&LOD.Indices[Section.FirstIndex], Section.IndexCount);
				Sections.Add(Kept);
			}
			LOD.Indices = MoveTemp(Indices);
			LOD.Sections = MoveTemp(Sections);
		}
	}

	FBox BoundsOf(const FMantleSkinnedMesh& Mesh)
	{
		FBox Box(ForceInit);
		if (Mesh.LODs.Num() > 0)
		{
			for (const FVector3f& P : Mesh.LODs[0].Positions) Box += FVector(P);
		}
		return Box;
	}

	/** Mesh built as the client builds it (FMantleRuntimeSkeletalMeshBuilder) on Skeleton; null, logged, on failure. */
	USkeletalMesh* BuildShown(const FMantleSkinnedMesh& Mesh, USkeleton* Skeleton, const FString& What)
	{
		FMantleSkinnedMeshBuildParams Params;
		Params.Skeleton = Skeleton;
		Params.bAllowMissingHelperBones = true;
		FString Error;
		USkeletalMesh* Built = FMantleRuntimeSkeletalMeshBuilder::Build(Mesh, Params, &Error);
		if (!Built) UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS %s could not be built: %s"), *What, *Error);
		return Built;
	}

	/** Materials by slot index, drawn in place of the built mesh's own. */
	TMap<int32, UMaterialInterface*> MaterialsBySlot(const TArray<FSkeletalMaterial>& Materials)
	{
		TMap<int32, UMaterialInterface*> Out;
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			if (Materials[Slot].MaterialInterface) Out.Add(Slot, Materials[Slot].MaterialInterface.Get());
		}
		return Out;
	}

	void Run(const TArray<FString>& Args)
	{
		UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS start"));
		if (Args.Num() < 5)
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: usage: Mantle.Dev.RenderLayers <Directory> <Label> <body> <Garment>@<slot>[:tucked][:tallboots] <Garment>@<slot> ... [size=<pixels>]"));
			return;
		}
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		FString WhyNot;
		if (!Contract || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: no contract, or nothing can render (%s)"), *WhyNot);
			return;
		}
		const FString& Directory = Args[0];
		const FString& Label = Args[1];
		const FString& BodyId = Args[2];
		int32 Size = 1024;
		EMetaHumanBodyType Type;
		EMantleFitSex Sex = EMantleFitSex::Auto;
		if (!FMantleFitBodies::ParseBodyType(BodyId, Type) || !FMantleEditorActions::ParseFitSex(BodyId.StartsWith(TEXT("m_")) ? TEXT("male") : TEXT("female"), Sex))
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %s is not a standard body id (m_med_nrw, f_med_unw ...)"), *BodyId);
			return;
		}
		FMantleFitBody Body;
		FString Error;
		if (!FMantleFitBodies::Make(BodyId, Type, Body, Error) || !Body.BodyMesh.IsValid())
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: the body %s could not be made (%s)"), *BodyId, *Error);
			return;
		}
		ON_SCOPE_EXIT { FMantleFitBodies::Release(Body); };
		FMantleSkinnedMesh BodyMesh;
		TArray<FSkeletalMaterial> BodyMaterials;
		if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Body.BodyMesh.Get(), BodyMesh, &Error, &BodyMaterials) || BodyMesh.LODs.IsEmpty())
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: the body could not be read (%s)"), *Error);
			return;
		}

		// Each garment fitted onto this body as CheckCosmetic fits it (RenderOutfitFits does the same per body).
		TArray<FWorn> Look;
		TArray<TStrongObjectPtr<UObject>> KeepAlive;
		for (int32 Index = 3; Index < Args.Num(); ++Index)
		{
			if (Args[Index].StartsWith(TEXT("size=")))
			{
				Size = FMath::Clamp(FCString::Atoi(*Args[Index].RightChop(5)), 128, 2048);
				continue;
			}
			FWorn Worn;
			Worn.Spec = Args[Index];
			TArray<FString> Parts;
			Worn.Spec.ParseIntoArray(Parts, TEXT(":"), true);
			FString Path, Slot;
			if (Parts.IsEmpty() || !Parts[0].Split(TEXT("@"), &Path, &Slot) || !Contract->IsClothingSlot(Slot))
			{
				UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %s is not <Garment>@<clothing slot>[:tucked][:tallboots]"), *Worn.Spec);
				return;
			}
			Worn.Slot = Slot;
			Worn.bTucked = Parts.Contains(TEXT("tucked"));
			Worn.bTallBoots = Parts.Contains(TEXT("tallboots"));
			if (!FPackageName::GetShortName(Path).Contains(TEXT("."))) Path += TEXT(".") + FPackageName::GetShortName(Path);
			UObject* Source = LoadObject<UObject>(nullptr, *Path);
			if (!Source || !(FMantleParametricOutfit::IsSource(Source) || FMantleParametricOutfit::IsGarmentSource(Source)))
			{
				UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %s is not an Outfit or a garment"), *Path);
				return;
			}
			FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Garment, Source, Slot, FString(), *Contract, Sex);
			TSharedPtr<void> WrapLease;
			if (FMantleParametricOutfit::IsGarmentSource(Source))
			{
				FMantleCosmeticDraft Wrapped;
				if (!FMantleParametricOutfit::PrepareWrapped(Draft, *Contract, Wrapped, KeepAlive, WrapLease, Error))
				{
					UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %s cannot be made in sizes: %s"), *Path, *Error);
					return;
				}
				Draft = MoveTemp(Wrapped);
			}
			const FMantleOutfitAuthoringTarget* Target = Draft.OutfitTargets.FindByPredicate([&BodyId](const FMantleOutfitAuthoringTarget& Entry) { return Entry.Id == BodyId; });
			if (!Target)
			{
				TArray<FString> Ids;
				for (const FMantleOutfitAuthoringTarget& Entry : Draft.OutfitTargets) Ids.Add(Entry.Id);
				UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %s has no fit on %s (it has: %s; %s)"), *Path, *BodyId, *FString::Join(Ids, TEXT(", ")), *Draft.FitBodiesStatus);
				return;
			}
			FMantleCosmeticDraft Input = Draft;
			Input.OutfitTargets.Reset();
			Input.OutfitSourceSize.Reset();
			Input.OutfitAuthoringCharacter = Target->Character;
			Input.bOutfitAuthoringInMemory = Target->bInMemory;
			Input.AuthoringBody = Target->Body;
			Input.AuthoringFace = Target->Face;
			FMantleCosmeticDraft Prepared;
			USkeletalMesh* Garment = FMantleParametricOutfit::Prepare(Input, *Contract, Prepared, KeepAlive, Error) ? Cast<USkeletalMesh>(Prepared.Source.Get()) : nullptr;
			Worn.Fitted = MakeShared<FMantleSkinnedMesh>();
			if (!Garment || !FMantleSkinnedMesh::ExtractFromSkeletalMesh(Garment, *Worn.Fitted, &Error, &Worn.Materials) || Worn.Fitted->LODs.IsEmpty())
			{
				UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %s could not be fitted onto %s (%s)"), *Path, *BodyId, *Error);
				return;
			}
			KeepSections(*Worn.Fitted, Prepared.KeepSlots);
			// Its skin mask on its source body (here the body it was fitted to), as the client derives it (MantleCatalogGarmentFit.cpp:56).
			Worn.Mask = MakeUnique<FMantleSkinMask>();
			FString MaskError;
			if (!FMantleSkinMask::Derive(FMantleGarmentSurface(Worn.Fitted->LODs[0]), BodyMesh.LODs[0].Positions, BodyMesh.LODs[0].Normals, BodyMesh.LODs[0].Indices,
				FMantleSkinMaskParams(), *Worn.Mask, &MaskError))
			{
				UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS %s: no skin mask (%s), the wearer fallback hides its skin"), *Worn.Spec, *MaskError);
				Worn.Mask.Reset();
			}
			Worn.Rank = RankOf(*Contract, Worn);
			UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS %s fitted onto %s: rank %d, %d triangles"), *Worn.Spec, *BodyId, Worn.Rank,
				Worn.Fitted->LODs[0].Indices.Num() / 3);
			Look.Add(MoveTemp(Worn));
		}
		if (Look.IsEmpty())
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: no garments"));
			return;
		}
		// From the skin outward, as the merged look orders them (ties keep the order given).
		Algo::StableSortBy(Look, [](const FWorn& Worn) { return Worn.Rank; });

		// The client's layer pass (MantleAvatarGarments.cpp StartDressed): placed from the skin outward, then the body under the placed
		// garments, then the garments as shown (their covered parts cut).
		TArray<FMantleLayerGarment> Layers;
		for (const FWorn& Worn : Look)
		{
			FMantleLayerGarment& Layer = Layers.AddDefaulted_GetRef();
			Layer.Mesh = Worn.Fitted;
			Layer.Rank = Worn.Rank;
		}
		TArray<TSharedPtr<const FMantleSkinnedMesh>> Placed, Shown;
		FMantleLayerFitStats LayerStats;
		FMantleLayerFit::Apply(Layers, Placed, LayerStats, &Shown);
		UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS layers: %d points pushed, %d triangles hidden, %d points sunk (%.1f ms)"),
			LayerStats.Pushed, LayerStats.Hidden, LayerStats.Sunk, LayerStats.Ms);
		TArray<FMantleGarmentSurface> Surfaces;
		TArray<FMantleDressedMask> Masks;
		for (int32 Index = 0; Index < Look.Num(); ++Index)
		{
			Surfaces.Add(FMantleGarmentSurface(Placed[Index]->LODs[0]));
			FMantleDressedMask& Mask = Masks.AddDefaulted_GetRef();
			Mask.Mask = Look[Index].Mask.Get();
			Mask.BorderCm = Look[Index].Slot == TEXT("footwear") || Look[Index].Slot == TEXT("socks") ? FMantleSkinMask::FootwearBorderCm : FMantleSkinMask::DefaultBorderCm;
		}
		FMantleDressedBody Dressed;
		if (!FMantleDressedBody::Compute(BodyMesh.LODs[0], Surfaces, FMantleDressedBodyParams::SkinMaskDefaults(), Dressed, &Error, Masks))
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: the body under the garments could not be computed (%s)"), *Error);
			return;
		}
		UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS body: %d vertices covered, %d triangles dropped, %d kept, %d sunk"),
			Dressed.CoveredVertices, Dressed.DroppedTriangles, Dressed.KeptTriangles, Dressed.Sink.Sunk);

		// Drawn as the client builds them: the dressed body, its face, every garment as shown, each with its own materials.
		USkeleton* Skeleton = Body.BodyMesh->GetSkeleton();
		USkeletalMesh* DressedBody = BuildShown(FMantleDressedBody::MakeDressedMesh(BodyMesh, Dressed), Skeleton, TEXT("the dressed body"));
		if (!DressedBody) return;
		KeepAlive.Emplace(DressedBody);
		FMantleThumbnailSubject Subject;
		Subject.SkeletalMeshes.Add(DressedBody);
		Subject.MaterialOverrides.Add(MaterialsBySlot(BodyMaterials));
		if (Body.FaceMesh.IsValid())
		{
			Subject.SkeletalMeshes.Add(Body.FaceMesh.Get());
			Subject.MaterialOverrides.AddDefaulted();
		}
		FBox Frame(ForceInit);
		for (int32 Index = 0; Index < Look.Num(); ++Index)
		{
			USkeletalMesh* Garment = BuildShown(*Shown[Index], Skeleton, Look[Index].Spec);
			if (!Garment) return;
			KeepAlive.Emplace(Garment);
			Subject.SkeletalMeshes.Add(Garment);
			Subject.MaterialOverrides.Add(MaterialsBySlot(Look[Index].Materials));
			Frame += BoundsOf(*Shown[Index]);
		}
		for (const TStrongObjectPtr<UObject>& Held : KeepAlive) Subject.KeepAlive.Add(Held);
		// Framed on the garments with some of the body around them, as GarmentSubject frames one garment.
		Subject.Frame = Frame.ExpandBy(FMath::Max(Frame.GetExtent().GetMax() * 0.2, 5.0));
		Subject.bFloor = true;
		Subject.SettleFrames = 30;
		Subject.Describe = FString::Printf(TEXT("%d garments layered on %s"), Look.Num(), *BodyId);

		const TPair<const TCHAR*, float> Views[] = { { TEXT("front"), 0.0f }, { TEXT("quarter"), 45.0f }, { TEXT("side"), 90.0f }, { TEXT("back"), 180.0f } };
		const FMantleThumbnailSubject Front = FMantleCosmeticThumbnail::GarmentSubject(DressedBody, nullptr, nullptr);
		int32 Failed = 0;
		for (const TPair<const TCHAR*, float>& View : Views)
		{
			FMantleThumbnailSubject Shot = Subject;
			Shot.View = Front.View;
			Shot.View.Yaw += View.Value;
			TArray<FColor> Pixels;
			const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s.png"), *Label, View.Key));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error) && FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
			Failed += bWritten ? 0 : 1;
			UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS %s %s %s"), View.Key, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
		if (Failed > 0)
		{
			UE_LOG(LogMantleRenderLayers, Error, TEXT("MANTLE_RENDER_LAYERS failed: %d of 4 pictures"), Failed);
			return;
		}
		UE_LOG(LogMantleRenderLayers, Display, TEXT("MANTLE_RENDER_LAYERS done"));
	}

	FAutoConsoleCommand GMantleDevRenderLayersCommand(
		TEXT("Mantle.Dev.RenderLayers"),
		TEXT("Mantle.Dev.RenderLayers <Directory> <Label> <body> <Garment>@<slot>[:tucked][:tallboots] ... [size=<pixels>] -- fits the garments onto a standard body ")
		TEXT("as CheckCosmetic does, layers them with the client's own steps (FMantleLayerFit, FMantleDressedBody with skin masks) and renders the look front, ")
		TEXT("quarter, side and back. Saves and uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
