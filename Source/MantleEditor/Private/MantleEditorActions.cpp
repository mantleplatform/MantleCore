// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEditorActions.h"

#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticValidation.h"
#include "MantleCreatorSignIn.h"
#include "MantleFitBodies.h"
#include "MantleParametricOutfit.h"
#include "MantlePublishPanel.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "MetaHumanCharacter.h"
#include "Misc/PackageName.h"

bool FMantleEditorActions::IsSignedIn()
{
	return FMantleCreatorSignIn::IsSignedIn();
}

void FMantleEditorActions::StartPublish(UWorld* World, const bool bGoLive, const FString& Notes)
{
	SMantlePublishPanel::Start(World, bGoLive, Notes);
}

TSharedPtr<FMantlePlacePublishSession> FMantleEditorActions::GetPublishSession()
{
	return SMantlePublishPanel::GetSession();
}

bool FMantleEditorActions::IsPublishRunning()
{
	return SMantlePublishPanel::IsRunning();
}

FMantleCosmeticDraft FMantleEditorActions::MakeCosmeticDraft(const EMantleCosmeticClass Class, UObject* Source, const FString& Slot,
	const FString& Name, const FMantleAvatarContract& Contract, const EMantleFitSex FitSex)
{
	// SetClass and a slot pick leave a fresh draft; SetSource then applies these in this order.
	FMantleCosmeticDraft Draft;
	Draft.Class = Class;
	Draft.Slot = Slot;
	Draft.Name = Name;
	Draft.Source = Source;
	Draft.FitSex = FitSex;
	ApplyCosmeticSourceDefaults(Draft, &Contract);
	ApplyCosmeticSectionDefaults(Draft);
	ApplyCosmeticSocketDefaults(Draft, &Contract);
	TArray<FName> JawCandidates;
	ApplyCosmeticJawDefaults(Draft, &Contract, JawCandidates);
	TArray<FName> SecondaryCandidates;
	ApplyCosmeticSecondaryDefaults(Draft, &Contract, SecondaryCandidates);
	ApplyCosmeticOutfitDefaults(Draft);
	ApplyCosmeticFitTargetDefaults(Draft, &Contract);
	ApplyCosmeticRoleDefaults(Draft, &Contract);
	return Draft;
}

void FMantleEditorActions::ApplyCosmeticSourceDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract)
{
	UObject* Source = Draft.Source.Get();
	if (Contract && Source && (Draft.Class == EMantleCosmeticClass::Garment || Draft.Class == EMantleCosmeticClass::Hair)
		&& !Draft.AuthoringBody.IsValid())
	{
		Draft.AuthoringBody = FMantleCosmeticExport::FindAuthoringBody(Source, *Contract, Draft.Class != EMantleCosmeticClass::Hair);
	}
	if (Source && Draft.Name.IsEmpty())
	{
		Draft.Name = Source->GetName();
	}
}

void FMantleEditorActions::ApplyCosmeticSectionDefaults(FMantleCosmeticDraft& Draft)
{
	if (Draft.Class != EMantleCosmeticClass::Hair)
	{
		return;
	}
	UObject* Source = Draft.Source.Get();
	// A groom keeps CardsEntry unset: the export then picks the finest cards LOD that fits the budget. An explicit choice wins.
	if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Source))
	{
		// Render sections of LOD 0, named by their material slot.
		const TArray<FString> Names = FMantleCosmeticExport::RenderSectionNames(Mesh, 0);
		for (int32 Section = 0; Section < Names.Num() && Draft.SourceSection == INDEX_NONE; ++Section)
		{
			if (Names[Section].Contains(TEXT("lash"), ESearchCase::IgnoreCase))
			{
				Draft.SourceSection = Section;
			}
		}
	}
}

void FMantleEditorActions::ApplyCosmeticSocketDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract)
{
	Draft.SocketOverrides.Reset();
	if (Draft.Class == EMantleCosmeticClass::Avatar && Contract)
	{
		Draft.SocketOverrides = FMantleCosmeticExport::DefaultSocketOverrides(Cast<USkeletalMesh>(Draft.Source.Get()), *Contract);
	}
}

FMantleCosmeticOutfitRows FMantleEditorActions::ApplyCosmeticOutfitDefaults(FMantleCosmeticDraft& Draft)
{
	FMantleCosmeticOutfitRows Rows;
	Draft.KeepSlots.Reset();
	if (USkeletalMesh* Mesh = Draft.Class == EMantleCosmeticClass::Garment ? Cast<USkeletalMesh>(Draft.Source.Get()) : nullptr)
	{
		Rows.Sections = FMantleCosmeticExport::ReadOutfitSections(Mesh, 0, Rows.Slots);
		Rows.Keep = FMantleOutfitSections::InitialKeep(Rows.Sections, Draft.Slot);
		Draft.KeepSlots = CosmeticKeepSlots(Rows.Keep, Rows.Slots);
	}
	return Rows;
}

void FMantleEditorActions::ApplyCosmeticFitTargetDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract)
{
	if (Draft.Class != EMantleCosmeticClass::Garment || !Draft.OutfitTargets.IsEmpty() || !FMantleParametricOutfit::IsSource(Draft.Source.Get()))
	{
		return;
	}
	Draft.FitBodies.Reset();
	Draft.FitBodiesStatus.Reset();
	Draft.FitBodyNotes.Reset();
	if (!Contract)
	{
		Contract = FMantleAvatarContract::Get();
	}
	if (!Contract)
	{
		Draft.FitBodiesStatus = TEXT("Garment variants were not made: the avatar contract is unavailable.");
		return;
	}
	// Epic's standard body types, made in memory (no converted Aera/Bo, no Epic sign-in). The set lives in the draft, so
	// the bodies last for the whole check and upload, and are released when the last copy of the draft goes.
	TSharedPtr<FMantleFitBodySet> Set;
	FString Error;
	if (!FMantleFitBodies::MakeSet(Draft.Source.Get(), Draft.FitSex, *Contract, Set, Error) || !Set.IsValid())
	{
		Draft.FitBodiesStatus = FString::Printf(TEXT("Garment variants were not made: %s."), *Error);
		UE_LOG(LogTemp, Warning, TEXT("MANTLE_FIT_BODIES %s"), *Draft.FitBodiesStatus);
		return;
	}
	TArray<FMantleOutfitAuthoringTarget> Targets;
	TArray<FString> Notes;
	for (const FMantleFitBody& Body : Set->Bodies)
	{
		// The source body shape must have Mantle's reference body topology, which only some of Creator's meshes/LODs carry.
		FString Lods;
		FString Ordering;
		USkeletalMesh* Shape = FMantleFitBodies::PickShapeBody(Body, *Contract, Lods, &Ordering);
		if (!Shape)
		{
			Draft.FitBodiesStatus = FString::Printf(TEXT("Garment variants were not made: %s has no body with Mantle's reference topology (%s)."), *Body.Id, *Lods);
			return;
		}
		Notes.Add(Ordering);
		// The face goes with it; the export puts it in the built face layout (FMantleFaceLayout), or leaves it out with a Warning.
		Targets.Add({ Body.Id, Body.Character.Get(), Shape, Body.FaceMesh.Get(), true });
	}
	Draft.OutfitTargets = MoveTemp(Targets);
	Draft.FitBodyNotes = MoveTemp(Notes);
	Draft.FitBodies = Set;
	Draft.FitBodiesStatus = Set->Status;
}

void FMantleEditorActions::SetCosmeticFitSex(FMantleCosmeticDraft& Draft, const EMantleFitSex Sex, const FMantleAvatarContract* Contract)
{
	Draft.FitSex = Sex;
	// The standard bodies are the ones made in memory; the creator's own fit sources stay, after the new ones.
	TArray<FMantleOutfitAuthoringTarget> Own = Draft.OutfitTargets.FilterByPredicate(
		[](const FMantleOutfitAuthoringTarget& Target) { return !Target.bInMemory; });
	Draft.OutfitTargets.Reset();
	Draft.FitBodies.Reset();
	Draft.FitBodiesStatus.Reset();
	Draft.FitBodyNotes.Reset();
	ApplyCosmeticFitTargetDefaults(Draft, Contract);
	Draft.OutfitTargets.Append(MoveTemp(Own));
}

bool FMantleEditorActions::ParseFitSex(const FString& Text, EMantleFitSex& OutSex)
{
	const FString Name = Text.TrimStartAndEnd();
	if (Name.IsEmpty() || Name.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		OutSex = EMantleFitSex::Auto;
		return true;
	}
	if (Name.Equals(TEXT("female"), ESearchCase::IgnoreCase))
	{
		OutSex = EMantleFitSex::Female;
		return true;
	}
	if (Name.Equals(TEXT("male"), ESearchCase::IgnoreCase))
	{
		OutSex = EMantleFitSex::Male;
		return true;
	}
	return false;
}

bool FMantleEditorActions::ApplyCosmeticJawDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract, TArray<FName>& OutCandidates)
{
	OutCandidates.Reset();
	Draft.JawBone = NAME_None;
	const USkeletalMesh* Body = Cast<USkeletalMesh>(Draft.Source.Get());
	if (Draft.Class != EMantleCosmeticClass::Avatar || !Body || !Contract)
	{
		return false;
	}
	// The skeleton the upload will get (the jaw pick never changes it): the picker only where it has the slot.
	FString SkeletonId;
	FString Error;
	const FMantleContractSkeleton* Skeleton = FMantleCosmeticExport::AvatarSkeletonId(Body, *Contract, SkeletonId, Error)
		? Contract->FindSkeleton(SkeletonId) : nullptr;
	if (!Skeleton || !FMantleCosmeticChecks::HasJawSlot(*Skeleton))
	{
		return false;
	}
	// The export's joints are the mesh's whole raw reference skeleton, parent-first.
	const TArray<FMantleSkinJointName> Joints = FMantleCosmeticChecks::JointsOf(Body->GetRefSkeleton());
	OutCandidates = FMantleCosmeticChecks::JawCandidates(Joints);
	Draft.JawBone = FMantleCosmeticChecks::PreselectJawBone(OutCandidates);
	return true;
}

bool FMantleEditorActions::ApplyCosmeticSecondaryDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract, TArray<FName>& OutCandidates)
{
	OutCandidates.Reset();
	Draft.SecondaryMotion.Reset();
	const USkeletalMesh* Body = Cast<USkeletalMesh>(Draft.Source.Get());
	if (Draft.Class != EMantleCosmeticClass::Avatar || !Body || !Contract)
	{
		return false;
	}
	// The skeleton the upload will get (a mapping never changes it): the section only where it has the slots.
	FString SkeletonId;
	FString Error;
	const FMantleContractSkeleton* Skeleton = FMantleCosmeticExport::AvatarSkeletonId(Body, *Contract, SkeletonId, Error)
		? Contract->FindSkeleton(SkeletonId) : nullptr;
	if (!Skeleton || !FMantleCosmeticChecks::HasSecondarySlots(*Skeleton))
	{
		return false;
	}
	const TArray<FMantleSkinJointName> Joints = FMantleCosmeticChecks::JointsOf(Body->GetRefSkeleton());
	OutCandidates = FMantleCosmeticChecks::SecondaryCandidates(Joints, *Skeleton);
	// The jaw picker's bone is the jaw, never a chain (ApplyCosmeticJawDefaults runs first).
	OutCandidates.Remove(Draft.JawBone);
	Draft.SecondaryMotion = FMantleCosmeticChecks::SuggestSecondaryMappings(Joints, *Skeleton);
	Draft.SecondaryMotion.RemoveAll([&Draft](const FMantleSecondaryMapping& Mapping) { return !Draft.JawBone.IsNone() && Mapping.StartBone == Draft.JawBone; });
	return true;
}

void FMantleEditorActions::ApplyCosmeticRoleDefaults(FMantleCosmeticDraft& Draft, const FMantleAvatarContract* Contract)
{
	USkeletalMesh* Body = Cast<USkeletalMesh>(Draft.Source.Get());
	if (Draft.Class == EMantleCosmeticClass::Avatar && Body && Contract)
	{
		Draft.SectionRoles = FMantleCosmeticExport::DefaultSectionRoles(Body, FMantleCosmeticExport::AvatarBodyLOD(Body, *Contract));
	}
}

TOptional<TArray<int32>> FMantleEditorActions::CosmeticKeepSlots(TConstArrayView<bool> Keep, TConstArrayView<int32> Slots)
{
	TArray<int32> Kept;
	bool bAll = true;
	for (int32 Index = 0; Index < Keep.Num() && Index < Slots.Num(); ++Index)
	{
		if (Keep[Index])
		{
			Kept.Add(Slots[Index]);
		}
		else
		{
			bAll = false;
		}
	}
	if (bAll)
	{
		return TOptional<TArray<int32>>();
	}
	return TOptional<TArray<int32>>(MoveTemp(Kept));
}

bool FMantleEditorActions::PrepareCosmeticBuild(FMantleCosmeticDraft& Draft, const EMantleAvatarRoute Route, TArray<FMantleCosmeticIssue>& OutBlockers)
{
	OutBlockers.Reset();
	Draft.AvatarPublicationMode = AvatarPublicationMode(Draft.Class, Route);
	if (Draft.Class == EMantleCosmeticClass::Avatar && !Draft.Face.IsValid())
	{
		Draft.Face = FindConvertedFace(Cast<USkeletalMesh>(Draft.Source.Get()));
	}
	if (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()) && Draft.OutfitTargets.IsEmpty())
	{
		OutBlockers.Add(FMantleCosmeticChecks::Error(FString(TEXT("Mantle could not make its standard fit bodies for this Outfit, so add at least one fit source in Parametric clothing before checking or uploading it. ")) + Draft.FitBodiesStatus));
	}
	return OutBlockers.IsEmpty();
}

EMantleAvatarRoute FMantleEditorActions::CosmeticRouteOf(const UObject* Body)
{
	UPackage* Package = Body ? Body->GetPackage() : nullptr;
	const FString* Route = Package ? Package->GetMetaData().FindValue(Body, FMantleAvatarConverter::RouteKey) : nullptr;
	return Route && Route->Equals(LexToString(EMantleAvatarRoute::AsIs), ESearchCase::CaseSensitive) ? EMantleAvatarRoute::AsIs : EMantleAvatarRoute::Convert;
}

FString FMantleEditorActions::AvatarPublicationMode(const EMantleCosmeticClass Class, const EMantleAvatarRoute Route)
{
	if (Class != EMantleCosmeticClass::Avatar)
	{
		return FString();
	}
	return Route == EMantleAvatarRoute::AsIs ? TEXT("as_is") : TEXT("converted");
}

USkeletalMesh* FMantleEditorActions::FindConvertedFace(const USkeletalMesh* Body)
{
	if (!FMantleAvatarConverter::IsConvertOutput(Body))
	{
		return nullptr;
	}
	const FString* Built = Body->GetPackage()->GetMetaData().FindValue(Body, FMantleAvatarConverter::BuiltKey);
	if (!Built || Built->IsEmpty())
	{
		return nullptr;
	}
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(**Built));
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		USkeletalMesh* Face = Cast<USkeletalMesh>(Asset.GetAsset());
		if (!Face || Face == Body || Face->GetRefSkeleton().FindBoneIndex(TEXT("FACIAL_C_FacialRoot")) == INDEX_NONE)
		{
			continue;
		}
		const FString* FaceBuilt = Face->GetPackage()->GetMetaData().FindValue(Face, FMantleAvatarConverter::BuiltKey);
		if (FaceBuilt && *FaceBuilt == *Built)
		{
			return Face;
		}
	}
	return nullptr;
}
