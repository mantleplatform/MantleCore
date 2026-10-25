// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPreview.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformTime.h"
#include "MantleAssetStore.h"
#include "MantleAvatarContract.h"
#include "MantleBodyCoverage.h"
#include "MantleBuiltInMannequin.h"
#include "MantleBodyShape.h"
#include "MantleBodyTopologyData.h"
#include "MantleCatalogGarmentFit.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticItem.h"
// The idle clip stays opaque here (UMantleClipAnimInstance reads and plays it).
#include "MantleClipAnimInstance.h"
#include "MantleGarmentFitter.h"
#include "MantleHairHeadFit.h"
#include "MantleRuntimeTextureBuilder.h"
#include "MantleSkinMask.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MantleNeckSamples.h"
#include "MantleRuntime.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleSkinnedMesh.h"
#include "MantleSkinnedMeshReader.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticPreviewPrivate
{
	const TCHAR* const MetaHumanSkeletonId = TEXT("metahuman");
	const TCHAR* const MetaHumanFaceSkeletonId = TEXT("metahuman_face");
	const TCHAR* const UE5MannequinSkeletonId = TEXT("ue5mannequin");
	const TCHAR* const UE4MannequinSkeletonId = TEXT("ue4mannequin");
	const TCHAR* const UEFNMannequinSkeletonId = TEXT("uefnmannequin");

	double NowMs()
	{
		return FPlatformTime::Seconds() * 1000.0;
	}

	uint32 CrcOf(const void* Data, const int64 Bytes, const uint32 Seed)
	{
		return Bytes > 0 ? FCrc::MemCrc32(Data, static_cast<int32>(Bytes), Seed) : Seed;
	}

	/** Array (Stride entries per vertex) in reference order: entry i from render vertex RenderOfReference[i]. Left as it is when its size does not fit. */
	template <typename ElementType>
	void PreviewToReferenceOrder(TArray<ElementType>& Array, TConstArrayView<int32> RenderOfReference, const int32 Stride)
	{
		if (Stride <= 0 || Array.Num() != RenderOfReference.Num() * Stride)
		{
			return;
		}
		TArray<ElementType> Reordered;
		Reordered.SetNumUninitialized(Array.Num());
		for (int32 Reference = 0; Reference < RenderOfReference.Num(); ++Reference)
		{
			for (int32 Slot = 0; Slot < Stride; ++Slot)
			{
				Reordered[Reference * Stride + Slot] = Array[RenderOfReference[Reference] * Stride + Slot];
			}
		}
		Array = MoveTemp(Reordered);
	}

	/**
	 * #620: a body LOD read from render data (FMantleSkinnedMesh::ExtractFromSkeletalMesh) put in its reference order, for a
	 * body whose reference topology is its mesh description's (FMantleCosmeticExport::HasReferenceTopology): every vertex
	 * attribute permuted, the triangles renumbered. False when RenderOfReference is not one entry per vertex.
	 */
	bool PreviewBodyToReferenceOrder(FMantleSkinnedMeshLOD& LOD, TConstArrayView<int32> RenderOfReference)
	{
		const int32 NumVertices = LOD.NumVertices();
		if (RenderOfReference.Num() != NumVertices)
		{
			return false;
		}
		TArray<int32> ReferenceOfRender;
		ReferenceOfRender.Init(INDEX_NONE, NumVertices);
		for (int32 Reference = 0; Reference < NumVertices; ++Reference)
		{
			const int32 Render = RenderOfReference[Reference];
			if (Render < 0 || Render >= NumVertices || ReferenceOfRender[Render] != INDEX_NONE)
			{
				return false;
			}
			ReferenceOfRender[Render] = Reference;
		}
		PreviewToReferenceOrder(LOD.Positions, RenderOfReference, 1);
		PreviewToReferenceOrder(LOD.Normals, RenderOfReference, 1);
		PreviewToReferenceOrder(LOD.Tangents, RenderOfReference, 1);
		PreviewToReferenceOrder(LOD.UVs, RenderOfReference, 1);
		PreviewToReferenceOrder(LOD.HairCards, RenderOfReference, 1);
		PreviewToReferenceOrder(LOD.InfluenceBones, RenderOfReference, LOD.NumInfluences);
		PreviewToReferenceOrder(LOD.InfluenceWeights, RenderOfReference, LOD.NumInfluences);
		for (uint32& Index : LOD.Indices)
		{
			Index = Index < static_cast<uint32>(NumVertices) ? static_cast<uint32>(ReferenceOfRender[static_cast<int32>(Index)]) : Index;
		}
		return true;
	}

	/** An MSK1 of Role, or the reason it is not one. Any thread: a stranger's bytes, the reader is strict. */
	TSharedPtr<const FMantleSkinnedMesh> ReadMesh(const TArray<uint8>& Bytes, const EMantleSkinnedMeshRole Role, const TCHAR* What,
		FString& OutSkeleton, FString& OutError)
	{
		FMantleSkinnedMeshReadResult Read = FMantleSkinnedMeshReader::Read(Bytes);
		if (!Read.bSuccess)
		{
			OutError = FString::Printf(TEXT("the avatar's %s could not be read: %s"), What, *Read.Error);
			return nullptr;
		}
		if (Read.Header.Role != Role)
		{
			OutError = FString::Printf(TEXT("the avatar's %s is cooked as role %d, not as an avatar %s"), What, static_cast<int32>(Read.Header.Role), What);
			return nullptr;
		}
		OutSkeleton = Read.Header.Skeleton;
		return MakeShared<FMantleSkinnedMesh>(MoveTemp(Read.Mesh));
	}

	/**
	 * #535: the avatar preview's fallback idle, the project's FMantleAvatarPreviewPlan::IdleAssetName (the Game Animation
	 * Sample's) on Driver's skeleton, found by name in the asset registry; the first by package path. Null without one.
	 */
	UAnimationAsset* FindProjectIdle(const USkeletalMesh* Driver)
	{
		const USkeleton* DriverSkeleton = Driver ? Driver->GetSkeleton() : nullptr;
		if (!DriverSkeleton)
		{
			return nullptr;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Sequences;
		Registry.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(), Sequences, true);
		TArray<FAssetData> Named;
		for (const FAssetData& Data : Sequences)
		{
			if (Data.AssetName.ToString().Equals(FMantleAvatarPreviewPlan::IdleAssetName, ESearchCase::CaseSensitive))
			{
				Named.Add(Data);
			}
		}
		Named.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		for (const FAssetData& Data : Named)
		{
			UAnimationAsset* Idle = Cast<UAnimationAsset>(Data.GetAsset());
			if (Idle && Idle->GetSkeleton() && DriverSkeleton->IsCompatibleForEditor(Idle->GetSkeleton()))
			{
				return Idle;
			}
		}
		return nullptr;
	}
}

// ---- Targets and inputs ----------------------------------------------------------------------------

FMantlePreviewTarget FMantlePreviewTarget::Source()
{
	return FMantlePreviewTarget();
}

FMantlePreviewTarget FMantlePreviewTarget::Catalog(const FString& ItemId)
{
	FMantlePreviewTarget Target;
	Target.Kind = EMantlePreviewTargetKind::CatalogAvatar;
	Target.AvatarId = ItemId.TrimStartAndEnd();
	return Target;
}

FString FMantlePreviewTarget::Key() const
{
	return Kind == EMantlePreviewTargetKind::SourceBody ? FString(TEXT("source")) : AvatarId;
}

FString FMantlePreviewTarget::Describe() const
{
	return Kind == EMantlePreviewTargetKind::SourceBody ? FString(TEXT("the body it was authored on"))
		: FString::Printf(TEXT("catalog avatar %s"), *AvatarId);
}

// ---- The target picker (#283) ----------------------------------------------------------------------

FMantlePreviewPick FMantlePreviewPick::Source()
{
	return FMantlePreviewPick();
}

FMantlePreviewPick FMantlePreviewPick::Typed(const FString& Id)
{
	FMantlePreviewPick Pick;
	Pick.Kind = EMantlePreviewPickKind::TypedId;
	Pick.TypedId = Id;
	return Pick;
}

TArray<FMantlePreviewPick> FMantlePreviewPick::Options()
{
	TArray<FMantlePreviewPick> Out;
	Out.Add(FMantlePreviewPick::Source());
	Out.Add(FMantlePreviewPick::Typed(FString()));
	return Out;
}

FMantlePreviewPick FMantlePreviewPick::Default()
{
	return FMantlePreviewPick::Source();
}

FMantlePreviewPick FMantlePreviewPick::Initial(const FMantlePreviewPick* Remembered)
{
	if (!Remembered)
	{
		return Default();
	}
	if (Remembered->Kind == EMantlePreviewPickKind::SourceBody || Remembered->Kind == EMantlePreviewPickKind::TypedId)
	{
		return *Remembered;
	}
	return Default();
}

bool FMantlePreviewPick::ToTarget(FMantlePreviewTarget& OutTarget, FString& OutError) const
{
	switch (Kind)
	{
	case EMantlePreviewPickKind::TypedId:
		if (TypedId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Type the avatar's id (itm_u followed by 32 hex digits) to preview on it.");
			return false;
		}
		OutTarget = FMantlePreviewTarget::Catalog(TypedId.TrimStartAndEnd());
		return true;
	case EMantlePreviewPickKind::SourceBody:
	default:
		OutTarget = FMantlePreviewTarget::Source();
		return true;
	}
}

FString FMantlePreviewPick::Label() const
{
	switch (Kind)
	{
	case EMantlePreviewPickKind::TypedId:
		return TEXT("Another avatar by id");
	case EMantlePreviewPickKind::SourceBody:
	default:
		return TEXT("The body it was made on");
	}
}

bool FMantlePreviewPick::IsSameRow(const FMantlePreviewPick& Other) const
{
	return Kind == Other.Kind;
}

// ---- The avatar preview's plan (#283) --------------------------------------------------------------

const TCHAR* const FMantleAvatarPreviewPlan::IdleClipFileName = TEXT("preview-idle-uefn.man1");
const TCHAR* const FMantleAvatarPreviewPlan::IdleAssetName = TEXT("M_Neutral_Stand_Idle_Loop");

bool FMantleAvatarPreviewPlan::ForSkeleton(const FString& SkeletonId, const bool bHasFace, FMantleAvatarPreviewPlan& Out, FString& OutError)
{
	using namespace MantleCosmeticPreviewPrivate;
	Out = FMantleAvatarPreviewPlan();
	// The plugin's retargeters, the copy the client loads (UMantleBodyAnimInstance::RetargeterPathForSkeleton). Kept
	// equal to it by hand: MantleCore cannot include client/Source.
	const TCHAR* Retargeter = nullptr;
	if (SkeletonId.Equals(MetaHumanSkeletonId, ESearchCase::CaseSensitive))
	{
		Retargeter = TEXT("/MantleCore/Avatars/Retarget/RTG_UEFN_to_Mantle_MetaHuman.RTG_UEFN_to_Mantle_MetaHuman");
		Out.bShowFace = bHasFace;
		if (!bHasFace)
		{
			Out.Notes.Add(TEXT("no face is chosen, so the MetaHuman body shows without its head"));
		}
	}
	else if (SkeletonId.Equals(UE5MannequinSkeletonId, ESearchCase::CaseSensitive))
	{
		Retargeter = TEXT("/MantleCore/Avatars/Retarget/RTG_UEFN_to_Mantle_UE5Mannequin.RTG_UEFN_to_Mantle_UE5Mannequin");
		if (bHasFace)
		{
			Out.Notes.Add(TEXT("a UE5 Mannequin avatar has no separate face, so the chosen face is not shown"));
		}
	}
	else if (SkeletonId.Equals(UE4MannequinSkeletonId, ESearchCase::CaseSensitive))
	{
		// #593: the third appearance skeleton, whole like the UE5 Mannequin.
		Retargeter = TEXT("/MantleCore/Avatars/Retarget/RTG_UEFN_to_Mantle_UE4Mannequin.RTG_UEFN_to_Mantle_UE4Mannequin");
		if (bHasFace)
		{
			Out.Notes.Add(TEXT("a UE4 Mannequin avatar has no separate face, so the chosen face is not shown"));
		}
	}
	else if (SkeletonId.Equals(UEFNMannequinSkeletonId, ESearchCase::CaseSensitive))
	{
		// #609: the fourth, the driver's own skeleton, whole like the UE5 Mannequin: posed through the driver's IK rig
		// onto itself, the same retarget node as the others.
		Retargeter = TEXT("/MantleCore/Avatars/Retarget/RTG_UEFN_to_Mantle_UEFNMannequin.RTG_UEFN_to_Mantle_UEFNMannequin");
		if (bHasFace)
		{
			Out.Notes.Add(TEXT("a UEFN Mannequin avatar has no separate face, so the chosen face is not shown"));
		}
	}
	else
	{
		OutError = FString::Printf(TEXT("Mantle poses no avatar body on the '%s' skeleton: an avatar is skinned to the MetaHuman body skeleton, the UE5 Mannequin skeleton, the UE4 Mannequin skeleton or the UEFN Mannequin skeleton."),
			*SkeletonId);
		return false;
	}
	Out.SkeletonId = SkeletonId;
	Out.RetargeterPath = Retargeter;
	return true;
}

FString FMantleAvatarPreviewInputs::DefaultIdleClipPath()
{
	const FString Directory = FMantleAvatarContract::DefaultDirectory();
	return Directory.IsEmpty() ? FString() : FPaths::Combine(Directory, FMantleAvatarPreviewPlan::IdleClipFileName);
}

FMantleAvatarPreviewInputs FMantleAvatarPreviewInputs::Gather(const FMantleAvatarPreviewPlan& Plan, USkeletalMesh* InDriver, const FString& IdleClipPath,
	const FString& DriverError)
{
	FMantleAvatarPreviewInputs Out;

	// The plugin's retargeter: in every project with the plugin.
	const FSoftObjectPath RetargeterPath(Plan.RetargeterPath);
	UObject* Loaded = !RetargeterPath.IsNull() && FPackageName::DoesPackageExist(RetargeterPath.GetLongPackageName()) ? RetargeterPath.TryLoad() : nullptr;
	const UClass* RetargeterClass = FindObject<UClass>(nullptr, TEXT("/Script/IKRig.IKRetargeter"));
	if (!Loaded)
	{
		Out.Missing.Add(FString::Printf(TEXT("the Mantle plugin's retargeter %s (reinstall the Mantle plugin)"), *Plan.RetargeterPath));
	}
	else if (!RetargeterClass || !Loaded->IsA(RetargeterClass))
	{
		Out.Missing.Add(FString::Printf(TEXT("%s is not an IK retargeter (reinstall the Mantle plugin)"), *Plan.RetargeterPath));
		Loaded = nullptr;
	}
	Out.Retargeter = Loaded;

	// The project's UEFN mannequin, else the one the plugin builds from its own IK rig (FMantleBuiltInMannequin).
	Out.Driver = InDriver;
	if (!InDriver)
	{
		Out.Missing.Add(FString::Printf(TEXT("a UEFN mannequin to copy the pose from: this project has none and the Mantle plugin could not make its own (%s; reinstall the Mantle plugin)"),
			DriverError.IsEmpty() ? TEXT("see the Output Log") : *DriverError));
		return Out;
	}
	const bool bBuiltIn = FMantleBuiltInMannequin::Is(InDriver);
	if (bBuiltIn)
	{
		Out.Notes.Add(TEXT("the pose comes from the Mantle plugin's own UEFN mannequin, as this project has none"));
	}

	// The idle: the plugin's clip, else the project's own.
	if (!IdleClipPath.IsEmpty() && FPaths::FileExists(IdleClipPath))
	{
		FString IdleError;
		Out.IdleClip = UMantleClipAnimInstance::ReadEmoteClipFile(IdleClipPath, IdleError);
		if (!Out.IdleClip.IsValid())
		{
			Out.Notes.Add(FString::Printf(TEXT("the plugin's idle %s cannot be read (%s)"), *FPaths::GetCleanFilename(IdleClipPath), *IdleError));
		}
	}
	if (!Out.IdleClip.IsValid())
	{
		// A project idle is on the project's mannequin skeleton, which the built-in mannequin's is not.
		Out.IdleAsset = bBuiltIn ? nullptr : MantleCosmeticPreviewPrivate::FindProjectIdle(InDriver);
		if (!Out.IdleAsset)
		{
			Out.Notes.Add(bBuiltIn
				? FString::Printf(TEXT("there is no idle to play, so the avatar holds the UEFN mannequin's A-pose (to see it idle, add SKM_UEFN_Mannequin and %s from the Game Animation Sample)"),
					FMantleAvatarPreviewPlan::IdleAssetName)
				: FString(TEXT("there is no idle to play, so the pose is the UEFN mannequin's reference pose")));
		}
	}
	return Out;
}

FString FMantleAvatarPreviewInputs::Status(const FMantleAvatarPreviewPlan& Plan, const bool bPosed, const bool bRetargetNode) const
{
	FString Line;
	TArray<FString> AllNotes = Plan.Notes;
	if (bPosed)
	{
		Line = FString::Printf(TEXT("Your %s avatar, posed by %s from the UEFN mannequin, as players' clients pose it."), *Plan.SkeletonId,
			*FPackageName::ObjectPathToObjectName(Plan.RetargeterPath));
		AllNotes.Append(Notes);
	}
	else if (!bRetargetNode)
	{
		Line = FString::Printf(TEXT("Your %s avatar in its reference pose: the IK Rig plugin, which poses avatars, is not loaded in this editor."), *Plan.SkeletonId);
	}
	else if (Missing.Num() > 0)
	{
		Line = FString::Printf(TEXT("Your %s avatar in its reference pose: it needs %s."), *Plan.SkeletonId, *FString::Join(Missing, TEXT(", and ")));
	}
	else
	{
		Line = FString::Printf(TEXT("Your %s avatar in its reference pose: the preview could not pose it (see the Output Log)."), *Plan.SkeletonId);
	}
	if (AllNotes.Num() > 0)
	{
		Line += FString::Printf(TEXT(" Note: %s."), *FString::Join(AllNotes, TEXT("; ")));
	}
	return Line;
}

bool FMantlePreviewGarment::IsValid() const
{
	return Garment.IsValid() && Garment->LODs.Num() > 0 && SourceBody.IsValid() && !Key.IsEmpty()
		&& (bHair ? SourceFace.IsValid() : SourceShape.IsValid());
}

bool FMantlePreviewGarment::FromHairAssets(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
	FMantlePreviewGarment& Out, FString& OutError)
{
	Out = FMantlePreviewGarment();
	FMantleCosmeticDraft PreviewDraft = Draft;
	PreviewDraft.bRenderThumbnail = false;
	TSharedRef<FMantleSkinnedMesh> Cards = MakeShared<FMantleSkinnedMesh>();
	TSharedRef<FMantleSkinnedMesh> SourceHead = MakeShared<FMantleSkinnedMesh>();
	const FMantleCosmeticPackage Package = FMantleCosmeticExport::Build(PreviewDraft, Contract, &Cards.Get(), &SourceHead.Get());
	if (Package.HasErrors())
	{
		for (const auto& Issue : Package.Issues) if (Issue.Severity == EMantleCosmeticSeverity::Error) { OutError = Issue.ToString(); break; }
		return false;
	}
	if (Cards->LODs.IsEmpty() || SourceHead->LODs.IsEmpty()) { OutError = TEXT("The hair export did not produce cards and an approved source head."); return false; }
	TSharedRef<FMantleSkinnedMesh> Body = MakeShared<FMantleSkinnedMesh>();
	if (!Draft.AuthoringBody.IsValid() || !FMantleSkinnedMesh::ExtractFromSkeletalMesh(Draft.AuthoringBody.Get(), Body.Get(), &OutError)) return false;
	Out.bHair = true;
	Out.bFacialHair = Draft.Slot != TEXT("hair");
	Out.Garment = Cards;
	Out.SourceFace = SourceHead;
	Out.SourceBody = Body;
	Out.Slot = Draft.Slot;
	FMantleBodyShape KeyShape;
	KeyShape.Positions = Body->LODs[0].Positions;
	Out.Key = MakeKey(*Cards, KeyShape, Draft.Slot);
	if (const auto* MeshAsset = Package.FindAsset(Package.MeshKey)) Out.Key += TEXT(":") + MeshAsset->Hash;
	if (const auto* HeadAsset = Package.FindAsset(Package.FaceMeshKey)) Out.Key += TEXT(":") + HeadAsset->Hash;
	const auto& HeadLOD = SourceHead->LODs[0];
	Out.Key += FString::Printf(TEXT(":head%08x:v%d"), MantleCosmeticPreviewPrivate::CrcOf(HeadLOD.Positions.GetData(), HeadLOD.Positions.Num() * sizeof(FVector3f), 0), FMantleHairHeadFit::Version);
	for (const auto& Ref : Package.Materials)
	{
		const bool bAtlas = Ref.Role == MantleHairCardsRole;
		const auto Profile = MantleCosmetic::NativeHairProfileFromString(Ref.NativeHairProfile);
		if (Profile == MantleCosmetic::ENativeHairProfile::Unsupported || (!bAtlas && Profile != MantleCosmetic::ENativeHairProfile::Artist))
		{
			OutError = TEXT("The preview native material profile is unsupported or is not on a hair_cards atlas."); return false;
		}
		const TCHAR* NativePath = MantleCosmetic::NativeHairParentPath(Profile, Out.bFacialHair);
		const TCHAR* Path = NativePath ? NativePath
			: (Out.bFacialHair ? TEXT("/Game/Mantle/Materials/Hair/MI_MantleHair_Facial.MI_MantleHair_Facial") : TEXT("/Game/Mantle/Materials/Hair/MI_MantleHair.MI_MantleHair"));
		// The hair materials are the game's: a creator project has none, so its preview draws plain engine-material cards.
		// Test locally shows the real look.
		UMaterialInterface* Parent = FPackageName::DoesPackageExist(FSoftObjectPath(Path).GetLongPackageName())
			? LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
		if (!Parent) Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		if (!Parent) { OutError = TEXT("The hair preview material is missing."); return false; }
		const FMantleCosmeticUploadAsset* Atlas = Package.FindAsset(Ref.AlbedoKey);
		FMantleTextureBuildParams Params;
		Params.Role = Ref.Role == MantleHairCardsRole ? EMantleTextureRole::Data : EMantleTextureRole::Color;
		Params.MaxDimension = Contract.HairMaxTextureSize;
		Params.AddressX = Params.AddressY = MantleCosmetic::RequiresNativeHairSettings(Profile) ? TA_Wrap : TA_Clamp;
		Params.LODGroup = TEXTUREGROUP_Character;
		UTexture2D* Texture = Atlas ? FMantleRuntimeTextureBuilder::BuildFromBytes(Atlas->Bytes, Params, &OutError) : nullptr;
		if (!Texture) { if (OutError.IsEmpty()) OutError = TEXT("The exported hair atlas is missing."); return false; }
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
		Material->SetTextureParameterValue(bAtlas ? TEXT("HairAtlas") : TEXT("BaseColor"), Texture);
		Material->SetScalarParameterValue(TEXT("UseHairAtlas"), bAtlas ? 1.f : 0.f);
		Material->SetScalarParameterValue(TEXT("UseCardCoords"), Cards->LODs[0].HairCards.Num() == Cards->LODs[0].NumVertices() ? 1.f : 0.f);
		Material->SetVectorParameterValue(TEXT("Tint"), Ref.HairColor.Get(FLinearColor::White));
        if (MantleCosmetic::RequiresNativeHairSettings(Profile))
        {
            const auto* TangentAsset = Package.FindAsset(Ref.NativeHairTangentKey);
            if (!Ref.NativeHair.IsSet() || !Ref.NativeHair->IsValid() || !TangentAsset)
            { OutError = TEXT("Native v2 preview requires its exact source settings and direction atlas."); return false; }
            FMantleTextureBuildParams DirectionParams = Params; DirectionParams.Role = EMantleTextureRole::Data;
            UTexture2D* Direction = FMantleRuntimeTextureBuilder::BuildFromBytes(TangentAsset->Bytes, DirectionParams, &OutError);
            if (!Direction) return false;
            Material->SetTextureParameterValue(TEXT("NativeTangent"), Direction);
            Material->SetScalarParameterValue(TEXT("NativeMelanin"), Ref.NativeHair->Melanin);
            Material->SetScalarParameterValue(TEXT("NativeRedness"), Ref.NativeHair->Redness);
            Material->SetVectorParameterValue(TEXT("NativeDyeColor"), Ref.NativeHair->DyeColor);
            Out.Key += TEXT(":") + TangentAsset->Hash + FString::Printf(TEXT(":native-settings-%08x"), Ref.NativeHair->GetIdentityHash());
        }
		Out.Materials.Add(FSkeletalMaterial(Material));
		// A changed atlas or tint replaces built materials even if the card positions did not change.
		Out.Key += TEXT(":") + Atlas->Hash + TEXT(":") + Ref.HairColor.Get(FLinearColor::White).ToString() + TEXT(":") + Ref.NativeHairProfile;
	}
	return Out.IsValid();
}

FString FMantlePreviewGarment::MakeKey(const FMantleSkinnedMesh& InGarment, const FMantleBodyShape& Shape, const FString& Slot)
{
	using namespace MantleCosmeticPreviewPrivate;
	uint32 Crc = 0;
	if (InGarment.LODs.Num() > 0)
	{
		const FMantleSkinnedMeshLOD& LOD = InGarment.LODs[0];
		Crc = CrcOf(LOD.Positions.GetData(), LOD.Positions.Num() * sizeof(FVector3f), Crc);
		Crc = CrcOf(LOD.Indices.GetData(), LOD.Indices.Num() * sizeof(uint32), Crc);
		Crc = CrcOf(LOD.InfluenceWeights.GetData(), LOD.InfluenceWeights.Num() * sizeof(LOD.InfluenceWeights[0]), Crc);
	}
	Crc = CrcOf(Shape.Positions.GetData(), Shape.Positions.Num() * sizeof(FVector3f), Crc);
	return FString::Printf(TEXT("%s:%08x:%d"), *Slot, Crc, InGarment.LODs.Num() > 0 ? InGarment.LODs[0].NumVertices() : 0);
}

void FMantlePreviewGarment::KeepSections(FMantleSkinnedMesh& Mesh, TConstArrayView<int32> KeepSlots)
{
	for (FMantleSkinnedMeshLOD& LOD : Mesh.LODs)
	{
		const int32 NumVertices = LOD.NumVertices();
		const int32 Slots = LOD.NumInfluences;
		TArray<int32> NewIndex;
		NewIndex.Init(INDEX_NONE, NumVertices);
		TArray<int32> SourceOf;
		TArray<uint32> Indices;
		TArray<FMantleSkinnedMeshSection> Sections;
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			if (!KeepSlots.Contains(Section.MaterialIndex))
			{
				continue;
			}
			FMantleSkinnedMeshSection Kept = Section;
			Kept.FirstIndex = Indices.Num();
			const int32 End = FMath::Min(Section.FirstIndex + Section.IndexCount, LOD.Indices.Num());
			for (int32 Index = Section.FirstIndex; Index + 2 < End; Index += 3)
			{
				const uint32 Corners[3] = { LOD.Indices[Index], LOD.Indices[Index + 1], LOD.Indices[Index + 2] };
				if (Corners[0] >= static_cast<uint32>(NumVertices) || Corners[1] >= static_cast<uint32>(NumVertices)
					|| Corners[2] >= static_cast<uint32>(NumVertices))
				{
					continue;
				}
				for (const uint32 Corner : Corners)
				{
					const int32 Vertex = static_cast<int32>(Corner);
					if (NewIndex[Vertex] == INDEX_NONE)
					{
						NewIndex[Vertex] = SourceOf.Num();
						SourceOf.Add(Vertex);
					}
					Indices.Add(static_cast<uint32>(NewIndex[Vertex]));
				}
			}
			Kept.IndexCount = Indices.Num() - Kept.FirstIndex;
			Sections.Add(Kept);
		}

		FMantleSkinnedMeshLOD Out;
		Out.NumInfluences = LOD.NumInfluences;
		Out.bFullPrecisionUVs = LOD.bFullPrecisionUVs;
		Out.bHighPrecisionTangents = LOD.bHighPrecisionTangents;
		Out.Indices = MoveTemp(Indices);
		Out.Sections = MoveTemp(Sections);
		const bool bTangents = LOD.Tangents.Num() == NumVertices;
		const bool bHairCards = LOD.HairCards.Num() == NumVertices;
		const bool bNormals = LOD.Normals.Num() == NumVertices;
		const bool bUVs = LOD.UVs.Num() == NumVertices;
		const bool bSkin = LOD.InfluenceBones.Num() == NumVertices * Slots && LOD.InfluenceWeights.Num() == NumVertices * Slots;
		for (const int32 Source : SourceOf)
		{
			Out.Positions.Add(LOD.Positions[Source]);
			if (bNormals)
			{
				Out.Normals.Add(LOD.Normals[Source]);
			}
			if (bTangents)
			{
				Out.Tangents.Add(LOD.Tangents[Source]);
			}
			if (bUVs)
			{
				Out.UVs.Add(LOD.UVs[Source]);
			}
			if (bHairCards)
			{
				Out.HairCards.Add(LOD.HairCards[Source]);
			}
			if (bSkin)
			{
				for (int32 Slot = 0; Slot < Slots; ++Slot)
				{
					Out.InfluenceBones.Add(LOD.InfluenceBones[Source * Slots + Slot]);
					Out.InfluenceWeights.Add(LOD.InfluenceWeights[Source * Slots + Slot]);
				}
			}
		}
		LOD = MoveTemp(Out);
	}
}

bool FMantlePreviewGarment::FromAssets(const USkeletalMesh* InGarment, const USkeletalMesh* Body, const FString& Slot,
	const FMantleAvatarContract& Contract, FMantlePreviewGarment& Out, FString& OutError, const USkeletalMesh* Face, const TArray<int32>* KeepSlots)
{
	Out = FMantlePreviewGarment();
	if (!InGarment)
	{
		OutError = TEXT("Choose the garment's skeletal mesh to preview it.");
		return false;
	}
	if (!Body)
	{
		OutError = TEXT("Choose the MetaHuman body the garment was made on (Authored on) to preview it.");
		return false;
	}
	TSharedRef<FMantleSkinnedMesh> GarmentMesh = MakeShared<FMantleSkinnedMesh>();
	FString Error;
	if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(InGarment, *GarmentMesh, &Error, &Out.Materials))
	{
		OutError = FString::Printf(TEXT("%s cannot be read for the preview: %s"), *InGarment->GetName(), *Error);
		return false;
	}
	if (KeepSlots)
	{
		// #483: only the sections the item uploads; the rest of the outfit and any skin are neither fitted nor shown.
		KeepSections(*GarmentMesh, *KeepSlots);
		if (GarmentMesh->LODs.Num() == 0 || GarmentMesh->LODs[0].NumTriangles() == 0)
		{
			OutError = FString::Printf(TEXT("no section of %s is chosen for this item: tick its sections under Sections to preview it."), *InGarment->GetName());
			return false;
		}
	}
	TSharedRef<FMantleBodyShape> Shape = MakeShared<FMantleBodyShape>();
	int32 ShapeLOD = FMantleCosmeticExport::FindReferenceLOD(Body, Contract);
	// #620: a catalog reference body or a body written in its order has its reference order in its mesh description; its
	// render data, which the preview reads, is in the editor build's order, so it is read through the bridge below.
	bool bDescriptionOrder = false;
	TArray<int32> RenderOfReference;
	if (ShapeLOD != INDEX_NONE)
	{
		FMantleCosmeticExport::HasReferenceTopology(Body, ShapeLOD, Contract, &bDescriptionOrder, &RenderOfReference);
	}
	if (!bDescriptionOrder && !FMantleBodyShape::FromSkeletalMesh(Body, Contract.TopologyVertexCount, Contract.TopologyUVHash, *Shape, &ShapeLOD, &Error))
	{
		OutError = FString::Printf(TEXT("the body %s has no LOD with Mantle's reference topology (%d vertices): %s"),
			*Body->GetName(), Contract.TopologyVertexCount, *Error);
		return false;
	}
	if (ShapeLOD != 0)
	{
		// The fit runs on the wearer's LOD0; the body must satisfy the canonical reference topology.
		OutError = FString::Printf(TEXT("the body %s has the reference topology at LOD %d, not LOD 0, so the preview cannot fit on it, though garments made on it still upload. Preview on a body exported with UE Optimized at Medium, whose LOD 0 is the reference topology."),
			*Body->GetName(), ShapeLOD);
		return false;
	}
	TSharedRef<FMantleSkinnedMesh> BodyMesh = MakeShared<FMantleSkinnedMesh>();
	if (!FMantleSkinnedMesh::ExtractFromSkeletalMesh(Body, *BodyMesh, &Error))
	{
		OutError = FString::Printf(TEXT("the body %s cannot be read for the preview: %s"), *Body->GetName(), *Error);
		return false;
	}
	if (bDescriptionOrder)
	{
		// LOD 0 in the reference order, and the shape from it, as FMantleBodyShape::FromSkeletalMesh makes one.
		if (BodyMesh->LODs.Num() == 0 || !MantleCosmeticPreviewPrivate::PreviewBodyToReferenceOrder(BodyMesh->LODs[0], RenderOfReference))
		{
			OutError = FString::Printf(TEXT("the body %s cannot be read in its reference order for the preview."), *Body->GetName());
			return false;
		}
		Shape->Positions = BodyMesh->LODs[0].Positions;
		Shape->PositionsHash = UMantleBodyTopologyData::ComputePositionsHash(Shape->Positions);
		Shape->ReferencePose = FMantleReferencePose::FromReferenceSkeleton(Body->GetRefSkeleton());
	}
	Out.Garment = GarmentMesh;
	Out.SourceShape = Shape;
	Out.SourceBody = BodyMesh;
	Out.Slot = Slot;
	Out.WarpPointCount = Contract.WarpPointCount;
	Out.TopologyVertexCount = Contract.TopologyVertexCount;
	Out.TopologyUVHash = Contract.TopologyUVHash;
	Out.Key = MakeKey(*GarmentMesh, *Shape, Slot);
	// #430: the face it was authored with, at its reference LOD (the one the upload sends for a MetaHuman face).
	FMantleSkinnedMeshLOD FaceLOD;
	if (Face && FMantleFaceTopology::ReadFace(Face, FaceLOD, nullptr, &Error))
	{
		const uint32 FaceCrc = MantleCosmeticPreviewPrivate::CrcOf(FaceLOD.Positions.GetData(), FaceLOD.Positions.Num() * sizeof(FVector3f), 0);
		TSharedRef<FMantleSkinnedMesh> FaceMesh = MakeShared<FMantleSkinnedMesh>();
		FaceMesh->LODs.Add(MoveTemp(FaceLOD));
		Out.SourceFace = FaceMesh;
		// Another face is another fit.
		Out.Key += FString::Printf(TEXT(":face%08x"), FaceCrc);
	}
	else if (Face)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW the face %s is left out (%s): collars are fitted from the body alone"), *Face->GetName(), *Error);
	}
	return true;
}

// ---- Lifetime --------------------------------------------------------------------------------------

TSharedRef<FMantleCosmeticPreviewSession> FMantleCosmeticPreviewSession::Create(const FMantleCosmeticPreviewSettings& InSettings,
	TSharedRef<IMantleCosmeticResolver> InResolver, TSharedRef<FMantleAssetStore> InStore)
{
	TSharedRef<FMantleCosmeticPreviewSession> Session = MakeShareable(new FMantleCosmeticPreviewSession(InSettings, InResolver, InStore));
	const TWeakPtr<FMantleCosmeticPreviewSession> Weak = Session;
	Session->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
	{
		if (const TSharedPtr<FMantleCosmeticPreviewSession> Pinned = Weak.Pin())
		{
			Pinned->Pump();
		}
		return true;
	}));
	return Session;
}

FMantleCosmeticPreviewSession::FMantleCosmeticPreviewSession(const FMantleCosmeticPreviewSettings& InSettings,
	TSharedRef<IMantleCosmeticResolver> InResolver, TSharedRef<FMantleAssetStore> InStore)
	: Settings(InSettings)
	, Resolver(MoveTemp(InResolver))
	, Store(MoveTemp(InStore))
	, Completions(MakeShared<FCompletionQueue, ESPMode::ThreadSafe>())
{
}

FMantleCosmeticPreviewSession::~FMantleCosmeticPreviewSession()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	for (const TPair<FString, TSharedRef<FWearer>>& Pair : Wearers)
	{
		if (Pair.Value->BodyFetch != 0)
		{
			Store->Cancel(Pair.Value->BodyFetch);
		}
		if (Pair.Value->FaceFetch != 0)
		{
			Store->Cancel(Pair.Value->FaceFetch);
		}
	}
}

void FMantleCosmeticPreviewSession::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (FSkeletalMaterial& Material : Garment.Materials) Collector.AddReferencedObject(Material.MaterialInterface);
	for (TPair<FString, TSharedRef<FWearer>>& Pair : Wearers)
	{
		Collector.AddReferencedObject(Pair.Value->BuiltBody);
		Collector.AddReferencedObject(Pair.Value->BuiltFace);
	}
	for (TPair<FString, TSharedRef<FFit>>& Pair : Fits)
	{
		Collector.AddReferencedObject(Pair.Value->BuiltGarment);
		Collector.AddReferencedObject(Pair.Value->BuiltBody);
	}
}

// ---- Requests --------------------------------------------------------------------------------------

void FMantleCosmeticPreviewSession::SetGarment(const FMantlePreviewGarment& InGarment)
{
	if (InGarment.Key != Garment.Key)
	{
		// Other inputs: their fits are someone else's. The avatars stay (downloaded once per session).
		Fits.Reset();
		WeightedShape.Reset();
	}
	bAvatarOnly = false;
	Garment = InGarment;
	if (Garment.SourceShape.IsValid() && Garment.SourceShape->HasWeights() && !WeightedShape.IsValid())
	{
		WeightedShape = Garment.SourceShape;
	}
}

void FMantleCosmeticPreviewSession::SetSourceMeshes(USkeletalMesh* Body, USkeletalMesh* Face)
{
	if (Settings.SourceBodyMesh.Get() != Body)
	{
		// Fits on the source body are built against its mesh.
		Fits.Remove(FitKeyFor(TEXT("source")));
	}
	Settings.SourceBodyMesh = Body;
	Settings.SourceFaceMesh = Face;
}

uint64 FMantleCosmeticPreviewSession::RequestAvatar(const FMantlePreviewTarget& InTarget)
{
	bAvatarOnly = true;
	return Request(InTarget);
}

uint64 FMantleCosmeticPreviewSession::Request(const FMantlePreviewTarget& InTarget)
{
	const FMantlePreviewTarget Previous = Target;
	const bool bWasActive = bActive;
	Target = InTarget;
	bActive = true;
	++RequestId;
	bWearerWasCached = false;
	bFitWasCached = false;
	if (bWasActive && Previous.Kind == EMantlePreviewTargetKind::CatalogAvatar && !(Previous == Target))
	{
		DropWearerIfUnwanted(Previous.AvatarId);
	}

	if (!bAvatarOnly && !Garment.IsValid())
	{
		Fail(TEXT("Choose the garment and the body it was made on to preview it."));
		return RequestId;
	}
	if (Target.Kind == EMantlePreviewTargetKind::CatalogAvatar)
	{
		if (!MantleCosmetic::IsItemId(Target.AvatarId))
		{
			Fail(FString::Printf(TEXT("'%s' is not an avatar id: catalog ids look like itm_u followed by 32 hex digits."), *Target.AvatarId));
			return RequestId;
		}
		const TSharedRef<FWearer>* Known = Wearers.Find(Target.AvatarId);
		if (Known && (*Known)->State == EWearerState::Ready)
		{
			bWearerWasCached = true;
		}
		else if (Known && (*Known)->State == EWearerState::Failed)
		{
			// Asked again: try again (a sign-in, an approval or the network may have changed since).
			Wearers.Remove(Target.AvatarId);
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW request=%llu target=%s garment=%s"), RequestId, *Target.Key(), *Garment.Key);
	Advance();
	return RequestId;
}

void FMantleCosmeticPreviewSession::Cancel()
{
	if (bActive && Target.Kind == EMantlePreviewTargetKind::CatalogAvatar)
	{
		DropWearerIfUnwanted(Target.AvatarId);
	}
	bActive = false;
	++RequestId;
	FMantlePreviewView Idle;
	Idle.Request = RequestId;
	SetView(MoveTemp(Idle));
}

void FMantleCosmeticPreviewSession::DropWearerIfUnwanted(const FString& Id)
{
	const TSharedRef<FWearer>* Found = Wearers.Find(Id);
	if (!Found || (*Found)->State == EWearerState::Ready || (*Found)->State == EWearerState::Failed)
	{
		return;
	}
	FWearer& Wearer = **Found;
	for (uint64* Fetch : { &Wearer.BodyFetch, &Wearer.FaceFetch })
	{
		if (*Fetch != 0)
		{
			Store->Cancel(*Fetch);
			*Fetch = 0;
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW dropped %s (another target was asked for)"), *Id);
	Wearers.Remove(Id);
}

FString FMantleCosmeticPreviewSession::FitKeyFor(const FString& WearerKey) const
{
	return Garment.Key + TEXT("|") + WearerKey;
}

FString FMantleCosmeticPreviewSession::CurrentFitKey() const
{
	if (Target.Kind == EMantlePreviewTargetKind::SourceBody)
	{
		return FitKeyFor(TEXT("source"));
	}
	const TSharedRef<FWearer>* Wearer = Wearers.Find(Target.AvatarId);
	return Wearer && (*Wearer)->State == EWearerState::Ready ? FitKeyFor((*Wearer)->Item.VersionKey()) : FString();
}

void FMantleCosmeticPreviewSession::Fail(const FString& Status)
{
	FMantlePreviewView Failed;
	Failed.Request = RequestId;
	Failed.Target = Target;
	Failed.State = EMantlePreviewState::Failed;
	Failed.Status = Status;
	if (View.State != EMantlePreviewState::Failed || View.Status != Status || View.Request != RequestId)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW failed request=%llu target=%s: %s"), RequestId, *Target.Key(), *Status);
	}
	SetView(MoveTemp(Failed));
}

void FMantleCosmeticPreviewSession::SetView(FMantlePreviewView&& NewView)
{
	const bool bChanged = NewView.Request != View.Request || NewView.State != View.State || NewView.Status != View.Status
		|| NewView.Body != View.Body || NewView.Face != View.Face || NewView.Garment != View.Garment;
	View = MoveTemp(NewView);
	if (bChanged && OnChanged)
	{
		OnChanged(View);
	}
}

// ---- The request's progress ------------------------------------------------------------------------

void FMantleCosmeticPreviewSession::Advance()
{
	if (!bActive)
	{
		return;
	}
	if (!bAvatarOnly && !Garment.IsValid())
	{
		Fail(TEXT("Choose the garment and the body it was made on to preview it."));
		return;
	}

	FMantlePreviewView Next;
	Next.Request = RequestId;
	Next.Target = Target;

	// The wearer: its CPU body (what the garment is fitted to) and the meshes the viewport shows.
	FString WearerKey;
	TSharedPtr<const FMantleSkinnedMesh> WearerBody;
	// #430: the wearer's face as CPU data, what the collar is fitted around (null: from the body alone).
	TSharedPtr<const FMantleSkinnedMesh> WearerFace;
	USkeletalMesh* WearerMesh = nullptr;
	USkeletalMesh* FaceMesh = nullptr;
	TArray<FString> Notes;
	if (Target.Kind == EMantlePreviewTargetKind::SourceBody)
	{
		WearerKey = TEXT("source");
		WearerBody = Garment.SourceBody;
		WearerFace = Garment.SourceFace;
		WearerMesh = Settings.SourceBodyMesh.Get();
		FaceMesh = Settings.SourceFaceMesh.Get();
		if (!WearerMesh)
		{
			Fail(TEXT("Choose the MetaHuman body the garment was made on (Authored on) to preview it."));
			return;
		}
		if (!FaceMesh)
		{
			Notes.Add(TEXT("no MetaHuman face was found next to the body"));
		}
	}
	else
	{
		TSharedRef<FWearer>* Found = Wearers.Find(Target.AvatarId);
		if (!Found)
		{
			StartWearer(Target.AvatarId);
			Found = Wearers.Find(Target.AvatarId);
			if (!Found)
			{
				return;
			}
		}
		FWearer& Wearer = **Found;
		// Cached avatar data is mode-independent; garment eligibility belongs to this request, not its download.
		if (!bAvatarOnly && !Garment.bHair && Wearer.State != EWearerState::Resolving && Wearer.State != EWearerState::Failed && !Wearer.Item.bGarmentCompatible)
		{
			Fail(FString::Printf(TEXT("%s takes no 3D clothing; players wearing it will not show this garment."), *Target.AvatarId));
			return;
		}
		if (!bAvatarOnly && !Garment.bHair && Wearer.State == EWearerState::Ready && Wearer.BodyUVHash != Garment.TopologyUVHash)
		{
			Fail(TEXT("The selected avatar does not have the garment's reference UV topology."));
			return;
		}
		switch (Wearer.State)
		{
		case EWearerState::Resolving:
			Next.State = EMantlePreviewState::Resolving;
			Next.Status = FString::Printf(TEXT("Looking up %s in the catalog..."), *Target.AvatarId);
			SetView(MoveTemp(Next));
			return;
		case EWearerState::Downloading:
		case EWearerState::Decoding:
		case EWearerState::Decoded:
			Next.State = EMantlePreviewState::Downloading;
			Next.Status = FString::Printf(TEXT("Downloading and reading %s's body%s..."), *Target.AvatarId,
				Wearer.Item.FaceMesh.IsLoadable() ? TEXT(" and face") : TEXT(""));
			SetView(MoveTemp(Next));
			return;
		case EWearerState::Failed:
			Fail(Wearer.Error);
			return;
		default:
			break;
		}
		WearerKey = Wearer.Item.VersionKey();
		WearerBody = Wearer.Body;
		WearerFace = Wearer.Face;
		WearerMesh = Wearer.BuiltBody;
		FaceMesh = Wearer.BuiltFace;
		Notes.Append(Wearer.Notes);
		Next.Sockets = Wearer.Item.Sockets;
	}

	if (bAvatarOnly)
	{
		Next.State = EMantlePreviewState::Ready;
		Next.Body = WearerMesh;
		Next.Face = FaceMesh;
		Next.Status = FString::Printf(TEXT("Accessory on %s. Check its clearance in motion; placement does not reshape the mesh."), *Target.Describe());
		if (Notes.Num() > 0) Next.Status += TEXT(" ") + FString::Join(Notes, TEXT("; "));
		SetView(MoveTemp(Next));
		return;
	}

	// An avatar may already be cached by a rigid preview, which does not require the garment topology.
	if (!WearerBody.IsValid() || WearerBody->LODs.Num() == 0 || (!Garment.bHair && (!Garment.SourceShape.IsValid()
		|| WearerBody->LODs[0].NumVertices() != Garment.SourceShape->Positions.Num())))
	{
		Fail(TEXT("The selected avatar does not have the garment's reference topology."));
		return;
	}
	// The fit.
	const FString FitKey = FitKeyFor(WearerKey);
	TSharedRef<FFit>* FoundFit = Fits.Find(FitKey);
	if (!FoundFit)
	{
		StartFit(FitKey, WearerBody, WearerFace);
		FoundFit = Fits.Find(FitKey);
		if (!FoundFit)
		{
			return;
		}
	}
	else if (View.Request != RequestId || View.State == EMantlePreviewState::Resolving || View.State == EMantlePreviewState::Downloading)
	{
		// This request found the fit already there (computed for an earlier request).
		bFitWasCached = true;
	}
	FFit& Fit = **FoundFit;
	if (Fit.State == EFitState::Fitting)
	{
		Next.State = EMantlePreviewState::Fitting;
		Next.Status = FString::Printf(TEXT("Fitting the garment to %s (off the editor thread)..."), *Target.Describe());
		SetView(MoveTemp(Next));
		return;
	}
	if (Fit.State == EFitState::Fitted)
	{
		Next.State = EMantlePreviewState::Building;
		BuildFit(Fit, WearerMesh, FaceMesh);
	}
	if (Fit.State == EFitState::Failed)
	{
		Fail(FString::Printf(TEXT("The garment could not be fitted to %s: %s"), *Target.Describe(), *Fit.Error));
		return;
	}
	if (Fit.State != EFitState::Ready)
	{
		return;
	}

	Next.State = EMantlePreviewState::Ready;
	Next.Body = Fit.BuiltBody ? Fit.BuiltBody.Get() : WearerMesh;
	Next.Face = FaceMesh;
	Next.Garment = Fit.BuiltGarment;
	Next.bGarmentFollowsFace = Garment.bHair && Fit.Fitted.IsValid() && FMantleHairHeadFit::IsFaceSkinned(*Fit.Fitted);
	Notes.Append(Fit.Notes);
	Next.Status = FString::Printf(TEXT("On %s: fitted by the %s path in %.0f ms%s%s."), *Target.Describe(), *Fit.Path, Fit.FitMs,
		bFitWasCached ? TEXT(" (already fitted)") : TEXT(""), bWearerWasCached ? TEXT(", avatar from the cache") : TEXT(""));
	if (Notes.Num() > 0)
	{
		Next.Status += FString::Printf(TEXT(" Note: %s."), *FString::Join(Notes, TEXT("; ")));
	}
	if (View.State != EMantlePreviewState::Ready || View.Request != RequestId)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW ready request=%llu target=%s path=%s fitMs=%.1f buildMs=%.1f hidden=%d wearerCached=%d fitCached=%d"),
			RequestId, *Target.Key(), *Fit.Path, Fit.FitMs, Fit.BuildMs, Fit.HiddenCount, bWearerWasCached ? 1 : 0, bFitWasCached ? 1 : 0);
	}
	SetView(MoveTemp(Next));
}

// ---- Catalog avatars -------------------------------------------------------------------------------

void FMantleCosmeticPreviewSession::StartWearer(const FString& Id)
{
	TSharedRef<FWearer> Wearer = MakeShared<FWearer>();
	Wearer->Id = Id;
	Wearer->State = EWearerState::Resolving;
	Wearers.Add(Id, Wearer);
	const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
	Resolver->Resolve({ Id }, [Queue, Id](const FMantleCosmeticResolveResult& Result)
	{
		FScopeLock Guard(&Queue->Lock);
		Queue->Items.Add([Id, Result](FMantleCosmeticPreviewSession& Session)
		{
			Session.OnResolved(Id, Result);
		});
	});
}

void FMantleCosmeticPreviewSession::OnResolved(const FString& Id, const FMantleCosmeticResolveResult& Result)
{
	const TSharedRef<FWearer>* Found = Wearers.Find(Id);
	if (!Found || (*Found)->State != EWearerState::Resolving)
	{
		return;
	}
	FWearer& Wearer = **Found;
	auto Refuse = [&Wearer](const FString& Why)
	{
		Wearer.State = EWearerState::Failed;
		Wearer.Error = Why;
	};
	if (!Result.bSuccess)
	{
		if (Result.StatusCode == 401)
		{
			Refuse(TEXT("Sign in to preview on catalog avatars (Tools > Mantle > Sign in with creator token)."));
		}
		else if (Result.StatusCode == 403)
		{
			Refuse(FString::Printf(TEXT("The Mantle API refused the catalog lookup for this sign-in: %s"), *Result.Error));
		}
		else
		{
			Refuse(FString::Printf(TEXT("Could not look up %s in the catalog: %s"), *Id, *Result.Error));
		}
		return;
	}
	const FMantleCosmeticItem* Item = Result.Items.FindByPredicate([&Id](const FMantleCosmeticItem& Candidate) { return Candidate.Id == Id; });
	if (!Item)
	{
		Refuse(FString::Printf(TEXT("The catalog does not serve %s: it is unknown, delisted or not approved yet."), *Id));
		return;
	}
	Wearer.Item = *Item;
	if (Item->Kind != EMantleCosmeticKind::Avatar)
	{
		Refuse(FString::Printf(TEXT("%s is a %s item, not an avatar."), *Id, MantleCosmetic::KindName(Item->Kind)));
		return;
	}
	if (Item->Skeleton != MantleCosmeticPreviewPrivate::MetaHumanSkeletonId)
	{
		Refuse(FString::Printf(TEXT("%s takes no 3D clothing (a %s avatar); players wearing it will not show this garment."), *Id,
			Item->Skeleton.IsEmpty() ? TEXT("non-MetaHuman") : *Item->Skeleton));
		return;
	}
	if (!Item->Mesh.IsPresent())
	{
		Refuse(FString::Printf(TEXT("%s has no approved body mesh yet."), *Id));
		return;
	}
	if (!Item->Mesh.IsLoadable())
	{
		Refuse(FString::Printf(TEXT("%s's body mesh has no cooked hash, so it cannot be verified and is not downloaded."), *Id));
		return;
	}
	if (Item->FaceMesh.IsPresent() && !Item->FaceMesh.IsLoadable())
	{
		Wearer.Notes.Add(TEXT("the avatar's face has no cooked hash, so it is not shown"));
	}
	else if (!Item->FaceMesh.IsPresent())
	{
		Wearer.Notes.Add(TEXT("the avatar has no face"));
	}

	Wearer.State = EWearerState::Downloading;
	const TWeakPtr<FMantleCosmeticPreviewSession> Weak = AsShared();
	auto FetchOne = [this, &Wearer, &Weak, &Id](const FMantleCosmeticAssetRef& Ref, const bool bFace) -> uint64
	{
		++Wearer.Pending;
		return Store->Request(Ref.ToStoreRef(), EMantleAssetLoad::Bytes, FMantleAssetDone::CreateLambda([Weak, Id, bFace](const FMantleAssetResult& Fetched)
		{
			if (const TSharedPtr<FMantleCosmeticPreviewSession> Pinned = Weak.Pin())
			{
				Pinned->OnFetched(Id, bFace, Fetched);
			}
		}));
	};
	Wearer.BodyFetch = FetchOne(Item->Mesh, false);
	if (Item->FaceMesh.IsLoadable())
	{
		Wearer.FaceFetch = FetchOne(Item->FaceMesh, true);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW resolved %s version=%d skeleton=%s face=%d; fetching"), *Id, Item->Version, *Item->Skeleton,
		Item->FaceMesh.IsLoadable() ? 1 : 0);
}

void FMantleCosmeticPreviewSession::OnFetched(const FString& Id, const bool bFace, const FMantleAssetResult& Result)
{
	const TSharedRef<FWearer>* Found = Wearers.Find(Id);
	if (!Found || (*Found)->State != EWearerState::Downloading)
	{
		return;
	}
	FWearer& Wearer = **Found;
	(bFace ? Wearer.FaceFetch : Wearer.BodyFetch) = 0;
	Wearer.Pending = FMath::Max(0, Wearer.Pending - 1);
	if (!Result.bSuccess || !Result.Bytes.IsValid())
	{
		if (bFace)
		{
			Wearer.Notes.Add(FString::Printf(TEXT("the avatar's face could not be downloaded (%s)"), *Result.Error));
		}
		else
		{
			Wearer.State = EWearerState::Failed;
			Wearer.Error = FString::Printf(TEXT("%s's body could not be downloaded (HTTP %d): %s"), *Id, Result.StatusCode, *Result.Error);
			if (Wearer.FaceFetch != 0)
			{
				Store->Cancel(Wearer.FaceFetch);
				Wearer.FaceFetch = 0;
			}
			Advance();
			return;
		}
	}
	else
	{
		(bFace ? Wearer.FaceBytes : Wearer.BodyBytes) = Result.Bytes;
	}
	if (Wearer.Pending > 0)
	{
		return;
	}

	// Both landed: read them off the game thread.
	Wearer.State = EWearerState::Decoding;
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> BodyBytes = Wearer.BodyBytes;
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> FaceBytes = Wearer.FaceBytes;
	Wearer.BodyBytes.Reset();
	Wearer.FaceBytes.Reset();
	Launch([Id, BodyBytes, FaceBytes]() -> FCompletion
	{
		using namespace MantleCosmeticPreviewPrivate;
		FString BodySkeleton;
		FString FaceSkeleton;
		FString Error;
		FString FaceNote;
		TSharedPtr<const FMantleSkinnedMesh> Body = BodyBytes.IsValid() ? ReadMesh(*BodyBytes, EMantleSkinnedMeshRole::AvatarBody, TEXT("body"), BodySkeleton, Error) : nullptr;
		TSharedPtr<const FMantleSkinnedMesh> Face;
		if (FaceBytes.IsValid())
		{
			Face = ReadMesh(*FaceBytes, EMantleSkinnedMeshRole::AvatarFace, TEXT("face"), FaceSkeleton, FaceNote);
		}
		return [Id, Body, Face, BodySkeleton, FaceSkeleton, Error, FaceNote](FMantleCosmeticPreviewSession& Session)
		{
			Session.OnDecoded(Id, Body, Face, BodySkeleton, FaceSkeleton, Error, FaceNote);
		};
	});
}

void FMantleCosmeticPreviewSession::OnDecoded(const FString& Id, TSharedPtr<const FMantleSkinnedMesh> Body, TSharedPtr<const FMantleSkinnedMesh> Face,
	const FString& BodySkeleton, const FString& FaceSkeleton, const FString& Error, const FString& FaceNote)
{
	const TSharedRef<FWearer>* Found = Wearers.Find(Id);
	if (!Found || (*Found)->State != EWearerState::Decoding)
	{
		return;
	}
	FWearer& Wearer = **Found;
	if (!Body.IsValid())
	{
		Wearer.State = EWearerState::Failed;
		Wearer.Error = Error.IsEmpty() ? FString(TEXT("the avatar's body could not be read")) : Error;
		return;
	}
	if (!FaceNote.IsEmpty())
	{
		Wearer.Notes.Add(FaceNote);
	}
	Wearer.BodyUVHash = Body->LODs.Num() > 0 ? UMantleBodyTopologyData::ComputeUVHash(Body->LODs[0].UVs) : 0;
	Wearer.Body = Body;
	Wearer.Face = Face;
	Wearer.BodySkeleton = BodySkeleton;
	Wearer.FaceSkeleton = FaceSkeleton;
	Wearer.State = EWearerState::Decoded;
	BuildWearer(Wearer);
}

USkeleton* FMantleCosmeticPreviewSession::SkeletonFor(const FString& SkeletonId) const
{
	using namespace MantleCosmeticPreviewPrivate;
	// A downloaded MetaHuman body or face binds by bone name to the creator's own MetaHuman skeletons.
	const USkeletalMesh* Source = SkeletonId == MetaHumanSkeletonId ? Settings.SourceBodyMesh.Get()
		: (SkeletonId == MetaHumanFaceSkeletonId ? Settings.SourceFaceMesh.Get() : nullptr);
	// The builder only reads the shared skeleton asset.
	return Source ? const_cast<USkeleton*>(Source->GetSkeleton()) : nullptr;
}

void FMantleCosmeticPreviewSession::BuildWearer(FWearer& Wearer)
{
	const double Start = MantleCosmeticPreviewPrivate::NowMs();
	USkeleton* Skeleton = SkeletonFor(Wearer.BodySkeleton);
	if (!Skeleton)
	{
		Wearer.State = EWearerState::Failed;
		Wearer.Error = FString::Printf(TEXT("This project has no '%s' skeleton to show the avatar on (the authoring MetaHuman body provides it)."), *Wearer.BodySkeleton);
		return;
	}
	FMantleSkinnedMeshBuildParams Params;
	Params.Skeleton = Skeleton;
	Params.Name = MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), FName(TEXT("SKM_MantlePreviewAvatar")));
	FString Error;
	USkeletalMesh* Body = FMantleRuntimeSkeletalMeshBuilder::Build(*Wearer.Body, Params, &Error);
	if (!Body)
	{
		Wearer.State = EWearerState::Failed;
		Wearer.Error = FString::Printf(TEXT("%s's body could not be built: %s"), *Wearer.Id, *Error);
		return;
	}
	Wearer.BuiltBody = Body;
	if (Wearer.Face.IsValid())
	{
		USkeleton* FaceSkeleton = SkeletonFor(Wearer.FaceSkeleton);
		if (!FaceSkeleton)
		{
			Wearer.Notes.Add(FString::Printf(TEXT("no '%s' skeleton here to show the avatar's face on"), *Wearer.FaceSkeleton));
		}
		else
		{
			FMantleSkinnedMeshBuildParams FaceParams;
			FaceParams.Skeleton = FaceSkeleton;
			FaceParams.Name = MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), FName(TEXT("SKM_MantlePreviewFace")));
			FString FaceError;
			Wearer.BuiltFace = FMantleRuntimeSkeletalMeshBuilder::Build(*Wearer.Face, FaceParams, &FaceError);
			if (!Wearer.BuiltFace)
			{
				Wearer.Notes.Add(FString::Printf(TEXT("the avatar's face could not be built (%s)"), *FaceError));
			}
		}
	}
	Wearer.Notes.Add(TEXT("the catalog avatar is shown without its textures"));
	Wearer.BuildMs = MantleCosmeticPreviewPrivate::NowMs() - Start;
	Wearer.State = EWearerState::Ready;
}

// ---- Fits ------------------------------------------------------------------------------------------

void FMantleCosmeticPreviewSession::StartFit(const FString& FitKey, const TSharedPtr<const FMantleSkinnedMesh>& WearerBody,
	const TSharedPtr<const FMantleSkinnedMesh>& WearerFace)
{
	TSharedRef<FFit> Fit = MakeShared<FFit>();
	Fits.Add(FitKey, Fit);
	const TSharedPtr<const FMantleSkinnedMesh> GarmentMesh = Garment.Garment;
	const TSharedPtr<const FMantleBodyShape> Shape = WeightedShape.IsValid() ? WeightedShape : Garment.SourceShape;
	const TSharedPtr<const FMantleSkinnedMesh> SourceFace = Garment.SourceFace;
	const bool bTracksBody = FMantleCatalogGarmentFit::TracksBody(Garment.Slot);
	const bool bFootwear = Garment.Slot.Equals(TEXT("footwear"), ESearchCase::CaseSensitive);
	const int32 WarpPoints = Garment.WarpPointCount;
	const int32 TopologyVertices = Garment.TopologyVertexCount;
	const FString GarmentKey = Garment.Key;
	if (Garment.bHair)
	{
		const bool bFacial = Garment.bFacialHair;
		Launch([FitKey, GarmentMesh, SourceFace, WearerBody, WearerFace, bFacial]() -> FCompletion
		{
			const double Start = MantleCosmeticPreviewPrivate::NowMs();
			FString Error;
			TSharedPtr<const FMantleSkinnedMesh> Result;
			TSharedRef<FMantleSkinnedMesh> Out = MakeShared<FMantleSkinnedMesh>();
			if (!SourceFace.IsValid() || !WearerFace.IsValid()) Error = TEXT("Hair fitting requires the approved source head and the selected avatar's MetaHuman face.");
			else if (FMantleHairHeadFit::Fit(*GarmentMesh, *SourceFace, *WearerFace, *WearerBody, bFacial, Out.Get(), Error)) Result = Out;
			const double Ms = MantleCosmeticPreviewPrivate::NowMs() - Start;
			return [FitKey, Result, Error, Ms](FMantleCosmeticPreviewSession& Session)
			{
				Session.OnFitted(FitKey, Result, {}, {}, 0, TEXT("MetaHuman head UV correspondence"), Error, {}, Ms);
			};
		});
		return;
	}
	Launch([FitKey, GarmentKey, GarmentMesh, Shape, SourceFace, WearerBody, WearerFace, bTracksBody, bFootwear, WarpPoints, TopologyVertices]() -> FCompletion
	{
		using namespace MantleCosmeticPreviewPrivate;
		const double Start = NowMs();
		TArray<FString> Notes;
		FString Error;
		TSharedPtr<const FMantleSkinnedMesh> Fitted;
		TSharedPtr<const FMantleSkinnedMesh> Dressed;
		TBitArray<> Hidden;
		int32 HiddenCount = 0;
		FString Path;

		// The shape's RBF weights, as the plugin computes them for the upload (once per garment inputs).
		TSharedPtr<const FMantleBodyShape> Weighted = Shape;
		if (Shape.IsValid() && !Shape->HasWeights())
		{
			TSharedRef<FMantleBodyShape> Copy = MakeShared<FMantleBodyShape>(*Shape);
			if (FMantleGarmentFitter::ComputeSourceWeights(*Copy, WarpPoints, TopologyVertices, &Error))
			{
				Weighted = Copy;
			}
			else
			{
				Weighted.Reset();
				Error = FString::Printf(TEXT("the body shape's warp weights could not be computed: %s"), *Error);
			}
		}

		// #430: the neck the upload's body shape carries, measured on the face the garment was authored with (once per
		// garment inputs, with the weights: seconds, as at upload).
		const FMantleSkinnedMeshLOD* SourceFaceLOD = SourceFace.IsValid() ? FMantleFaceTopology::FitLOD(*SourceFace) : nullptr;
		if (Weighted.IsValid() && SourceFaceLOD && !Weighted->Neck.IsSet())
		{
			TSharedRef<FMantleBodyShape> WithNeck = MakeShared<FMantleBodyShape>(*Weighted);
			FString NeckError;
			if (FMantleCatalogGarmentFit::ComputeNeck(*Weighted, *SourceFaceLOD, WarpPoints, WithNeck->Neck, &NeckError))
			{
				Weighted = WithNeck;
			}
			else
			{
				Notes.Add(FString::Printf(TEXT("the neck could not be measured on the face (%s), so the collar is fitted from the body alone"), *NeckError));
			}
		}
		const FMantleSkinnedMeshLOD* WearerFaceLOD = WearerFace.IsValid() ? FMantleFaceTopology::FitLOD(*WearerFace) : nullptr;

		// The client's fit: the field, the authored clearance for a garment that tracks the body, the fit.
		TSharedPtr<const FMantleWarpField> Field;
		if (Weighted.IsValid() && GarmentMesh.IsValid() && WearerBody.IsValid())
		{
			Field = FMantleCatalogGarmentFit::ComputeField(*Weighted, *WearerBody, Error, WearerFaceLOD);
		}
		// #877: the skin the garment hides on its own source body, kept for the body below (null: the wearer fallback).
		TSharedPtr<const FMantleSkinMask> SkinMask;
		if (Field.IsValid())
		{
			FMantleGarmentAuthoredClearance Authored;
			bool bAuthored = false;
			if (bTracksBody)
			{
				FString AuthoredError;
				bAuthored = FMantleCatalogGarmentFit::DeriveAuthoredClearance(*GarmentMesh, *Weighted, *WearerBody, Authored, &AuthoredError, SourceFaceLOD);
				if (!bAuthored)
				{
					// The client does the same: without its clearance the garment warps.
					Notes.Add(FString::Printf(TEXT("no authored clearance (%s), so the garment warps without the restore"), *AuthoredError));
				}
				else
				{
					SkinMask = Authored.SkinMask;
				}
			}
			TSharedRef<FMantleSkinnedMesh> Out = MakeShared<FMantleSkinnedMesh>();
			FMantleCatalogFitResult Result;
			if (FMantleCatalogGarmentFit::Fit(*GarmentMesh, *Field, *WearerBody, bAuthored ? &Authored : nullptr, *Out, &Error, &Result, WearerFaceLOD))
			{
				Fitted = Out;
			}
			Path = Result.Describe();
			if (bTracksBody && SourceFaceLOD && !WearerFaceLOD)
			{
				Notes.Add(TEXT("the wearer has no face, so the collar is fitted from the body alone"));
			}
		}
		else if (Error.IsEmpty())
		{
			Error = TEXT("the warp field could not be computed");
		}

		// The body under it, as the client's assembler hides and sinks it (top, bottom and footwear cover the body).
		if (Fitted.IsValid() && bTracksBody && WearerBody->LODs.Num() > 0 && Fitted->LODs.Num() > 0)
		{
			TArray<FMantleGarmentSurface> Surfaces;
			Surfaces.Add(FMantleGarmentSurface(Fitted->LODs[0]));
			FMantleDressedBody Body;
			FString DressedError;
			// #877: the client's rule (MantleAvatarGarments), with this garment's skin mask.
			FMantleDressedMask Mask;
			Mask.Mask = SkinMask.Get();
			Mask.BorderCm = bFootwear ? FMantleSkinMask::FootwearBorderCm : FMantleSkinMask::DefaultBorderCm;
			const FMantleDressedMask Masks[] = { Mask };
			if (FMantleDressedBody::Compute(WearerBody->LODs[0], Surfaces, FMantleDressedBodyParams::SkinMaskDefaults(), Body, &DressedError, Masks))
			{
				Dressed = MakeShared<FMantleSkinnedMesh>(FMantleDressedBody::MakeDressedMesh(*WearerBody, Body));
				Hidden = Body.Hidden;
				HiddenCount = Body.CoveredVertices;
			}
			else
			{
				Notes.Add(FString::Printf(TEXT("the body under the garment is not hidden (%s)"), *DressedError));
			}
		}
		const double Ms = NowMs() - Start;
		const TSharedPtr<const FMantleBodyShape> NewWeights = Weighted != Shape ? Weighted : TSharedPtr<const FMantleBodyShape>();
		return [FitKey, GarmentKey, Fitted, Dressed, Hidden, HiddenCount, Path, Error, Notes, Ms, NewWeights](FMantleCosmeticPreviewSession& Session)
		{
			if (NewWeights.IsValid() && Session.Garment.Key == GarmentKey
				&& (!Session.WeightedShape.IsValid() || (NewWeights->Neck.IsSet() && !Session.WeightedShape->Neck.IsSet())))
			{
				Session.WeightedShape = NewWeights;
			}
			Session.OnFitted(FitKey, Fitted, Dressed, Hidden, HiddenCount, Path, Error, Notes, Ms);
		};
	});
}

void FMantleCosmeticPreviewSession::OnFitted(const FString& FitKey, TSharedPtr<const FMantleSkinnedMesh> Fitted, TSharedPtr<const FMantleSkinnedMesh> Dressed,
	TBitArray<> Hidden, const int32 HiddenCount, const FString& Path, const FString& Error, const TArray<FString>& Notes, const double Ms)
{
	const TSharedRef<FFit>* Found = Fits.Find(FitKey);
	if (!Found || (*Found)->State != EFitState::Fitting)
	{
		// Inputs replaced meanwhile (SetGarment dropped the record).
		return;
	}
	FFit& Fit = **Found;
	// A result for a request that was replaced is kept: asking for that target again shows it at once.
	const bool bCurrent = bActive && CurrentFitKey() == FitKey;
	Fit.FitMs = Ms;
	Fit.Path = Path;
	Fit.Notes = Notes;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW fit %s: %s path=%s ms=%.1f hidden=%d%s"), *FitKey, Fitted.IsValid() ? TEXT("done") : TEXT("failed"),
		*Path, Ms, HiddenCount, bCurrent ? TEXT("") : TEXT(" (stale: kept, not shown)"));
	if (!Fitted.IsValid())
	{
		Fit.State = EFitState::Failed;
		Fit.Error = Error.IsEmpty() ? FString(TEXT("the garment could not be fitted")) : Error;
		return;
	}
	Fit.Fitted = Fitted;
	Fit.Dressed = Dressed;
	Fit.Hidden = MoveTemp(Hidden);
	Fit.HiddenCount = HiddenCount;
	Fit.State = EFitState::Fitted;
}

void FMantleCosmeticPreviewSession::BuildFit(FFit& Fit, USkeletalMesh* WearerMesh, USkeletalMesh* WearerFace)
{
	const bool bFaceHair = Garment.bHair && Fit.Fitted.IsValid() && FMantleHairHeadFit::IsFaceSkinned(*Fit.Fitted);
	if (bFaceHair) WearerMesh = WearerFace;
	if (!WearerMesh || !WearerMesh->GetSkeleton() || !Fit.Fitted.IsValid())
	{
		Fit.State = EFitState::Failed;
		Fit.Error = TEXT("the wearer's mesh is gone");
		return;
	}
	const double Start = MantleCosmeticPreviewPrivate::NowMs();
	FMantleSkinnedMeshBuildParams Params;
	Params.Skeleton = WearerMesh->GetSkeleton();
	// Built against the wearer's bones and pose, as the client builds it (leader pose hands it the wearer's).
	Params.ReferencePose = bFaceHair ? nullptr : &WearerMesh->GetRefSkeleton();
	Params.bAllowMissingHelperBones = !bFaceHair;
	Params.Materials = Garment.Materials;
	Params.Name = MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), FName(TEXT("SKM_MantlePreviewFit")));
	FString Error;
	USkeletalMesh* Built = FMantleRuntimeSkeletalMeshBuilder::Build(*Fit.Fitted, Params, &Error);
	if (!Built)
	{
		Fit.State = EFitState::Failed;
		Fit.Error = FString::Printf(TEXT("the fitted garment could not be built: %s"), *Error);
		return;
	}
	Fit.BuiltGarment = Built;
	if (Fit.Dressed.IsValid() && Fit.Hidden.Num() == Fit.Dressed->LODs[0].NumVertices())
	{
		FMantleSkinnedMeshBuildParams BodyParams;
		BodyParams.Skeleton = WearerMesh->GetSkeleton();
		BodyParams.HiddenVertices = &Fit.Hidden;
		BodyParams.Materials = WearerMesh->GetMaterials();
		BodyParams.Name = MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass(), FName(TEXT("SKM_MantlePreviewDressed")));
		FString BodyError;
		Fit.BuiltBody = FMantleRuntimeSkeletalMeshBuilder::Build(*Fit.Dressed, BodyParams, &BodyError);
		if (!Fit.BuiltBody)
		{
			Fit.Notes.Add(FString::Printf(TEXT("the body under the garment is not hidden (%s)"), *BodyError));
		}
	}
	// The builder keeps a CPU copy; the fit keeps only what it built.
	Fit.Dressed.Reset();
	Fit.BuildMs = MantleCosmeticPreviewPrivate::NowMs() - Start;
	Fit.State = EFitState::Ready;
}

// ---- Work ------------------------------------------------------------------------------------------

void FMantleCosmeticPreviewSession::Launch(TUniqueFunction<FCompletion()> Work)
{
	// The task holds the completion queue, never the session: closing the panel meanwhile does not keep it alive.
	const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
	UE::Tasks::Launch(TEXT("MantleCosmeticPreview"), [Queue, Work = MoveTemp(Work)]() mutable
	{
		FCompletion Completion = Work();
		FScopeLock Guard(&Queue->Lock);
		Queue->Items.Add([Completion](FMantleCosmeticPreviewSession& Session)
		{
			if (Completion)
			{
				Completion(Session);
			}
		});
	}, UE::Tasks::ETaskPriority::BackgroundNormal);
}

void FMantleCosmeticPreviewSession::DrainCompletions()
{
	TArray<FCompletion> Ready;
	{
		FScopeLock Guard(&Completions->Lock);
		Ready = MoveTemp(Completions->Items);
		Completions->Items.Reset();
	}
	for (FCompletion& Completion : Ready)
	{
		Completion(*this);
	}
}

void FMantleCosmeticPreviewSession::Pump()
{
	if (!IsInGameThread())
	{
		return;
	}
	bool bAny = false;
	{
		FScopeLock Guard(&Completions->Lock);
		bAny = Completions->Items.Num() > 0;
	}
	if (!bAny)
	{
		return;
	}
	DrainCompletions();
	Advance();
}
