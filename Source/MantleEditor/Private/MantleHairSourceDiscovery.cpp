// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairSourceDiscovery.h"
#include "MantleCosmeticExport.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace MantleHairSourceDiscoveryPrivate
{
	UObject* ObjectProperty(const UObject* Object, const TCHAR* Name)
	{
		const FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(),Name) : nullptr;
		return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
	}
	FString Meta(const UObject* Object, const TCHAR* Key)
	{
		return Object ? Object->GetOutermost()->GetMetaData().GetValue(Object,Key) : FString();
	}
}

bool FMantleHairSourceDiscovery::IsHead(const USkeletalMesh* Mesh)
{
	return Mesh && Mesh->GetRefSkeleton().FindBoneIndex(TEXT("head")) != INDEX_NONE
		&& Mesh->GetRefSkeleton().FindBoneIndex(TEXT("FACIAL_C_FacialRoot")) != INDEX_NONE;
}

bool FMantleHairSourceDiscovery::ChooseHead(const FString& Groom, TConstArrayView<FMantleHairBindingHead> Bindings, FString& Head, FString& Error)
{
	Head.Reset(); Error.Reset();
	for (const auto& Binding : Bindings)
	{
		if (Binding.Groom != Groom) continue;
		if (!Binding.SourceHead.IsEmpty() && !Binding.TargetHead.IsEmpty() && Binding.SourceHead != Binding.TargetHead)
		{ Error = TEXT("the Groom binding has different source and target heads; explicitly select the head whose cards were authored against"); Head.Reset(); return false; }
		const FString& Candidate = Binding.SourceHead.IsEmpty() ? Binding.TargetHead : Binding.SourceHead;
		if (Candidate.IsEmpty()) continue;
		if (!Head.IsEmpty() && Head != Candidate)
		{ Error = TEXT("the Groom is bound to multiple heads; explicitly select the head whose cards were authored against"); Head.Reset(); return false; }
		Head = Candidate;
	}
	if (Head.IsEmpty()) Error = TEXT("no exact authoring-head association was found; select the head the cards were aligned to");
	return !Head.IsEmpty();
}

USkeletalMesh* FMantleHairSourceDiscovery::FindHead(const UObject* Source, FString& Error, bool* HasAssociation)
{
	using namespace MantleHairSourceDiscoveryPrivate;
	Error.Reset();
	if (HasAssociation) *HasAssociation = false;
	if (!Source) { Error = TEXT("select hair cards or a Groom first"); return nullptr; }
	if (const FString HeadPath = Meta(Source,AuthoringHeadTag); !HeadPath.IsEmpty())
	{
		if (HasAssociation) *HasAssociation = true;
		USkeletalMesh* Head = LoadObject<USkeletalMesh>(nullptr,*HeadPath);
		if (IsHead(Head)) return Head;
		Error = TEXT("the recorded hair authoring head is missing or has no MetaHuman facial reference rig"); return nullptr;
	}
	const UObject* Groom = FMantleCosmeticExport::IsGroomAsset(Source) ? Source : nullptr;
	if (const FString OriginalPath = Meta(Source,OriginalGroomTag); !OriginalPath.IsEmpty())
	{
		if (HasAssociation) *HasAssociation = true;
		Groom = LoadObject<UObject>(nullptr,*OriginalPath);
		if (!FMantleCosmeticExport::IsGroomAsset(Groom))
		{ Error = TEXT("the recorded original Groom is missing; select the actual authoring head explicitly"); return nullptr; }
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (!Groom && Source->IsA<UStaticMesh>())
	{
		// A cards mesh may be referenced by one exact Groom, rather than merely sharing its filename.
		TArray<FName> Referencers;
		Registry.GetReferencers(Source->GetOutermost()->GetFName(),Referencers,UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName Package : Referencers)
		{
			TArray<FAssetData> Assets; Registry.GetAssetsByPackageName(Package,Assets);
			for (const auto& Data : Assets)
			{
				if (Data.AssetClassPath.GetAssetName() != FName(TEXT("GroomAsset"))) continue;
				UObject* Candidate = Data.GetAsset(); bool bReferencesCards = false;
				for (const auto& Entry : FMantleCosmeticExport::ReadGroomCards(Candidate)) if (Entry.Mesh.Get() == Source) bReferencesCards = true;
				if (!bReferencesCards) continue;
				if (HasAssociation) *HasAssociation = true;
				if (Groom && Groom != Candidate) { Error = TEXT("multiple Grooms reference these cards; select their actual authoring head explicitly"); return nullptr; }
				Groom = Candidate;
			}
		}
	}
	if (!Groom) { Error = TEXT("the cards have no recorded authoring head or original Groom; select the head they were aligned to"); return nullptr; }
	// A mesh can reference a generated Groom which retains provenance to the original strand asset.
	TSet<FString> ProvenancePaths;
	while (true)
	{
		if (ProvenancePaths.Contains(Groom->GetPathName()))
		{ Error = TEXT("the recorded original Groom provenance contains a cycle; select the actual authoring head explicitly"); return nullptr; }
		ProvenancePaths.Add(Groom->GetPathName());
		const FString OriginalPath = Meta(Groom,OriginalGroomTag);
		if (OriginalPath.IsEmpty()) break;
		if (HasAssociation) *HasAssociation = true;
		Groom = LoadObject<UObject>(nullptr,*OriginalPath);
		if (!FMantleCosmeticExport::IsGroomAsset(Groom))
		{ Error = TEXT("the recorded original Groom is missing; select the actual authoring head explicitly"); return nullptr; }
	}
	TArray<FAssetData> Bindings;
	Registry.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/HairStrandsCore.GroomBindingAsset")),Bindings,true);
	TArray<FMantleHairBindingHead> Evidence;
	for (const auto& Data : Bindings)
	{
		FString Tag; Data.GetTagValue(TEXT("Groom"),Tag);
		if (FPackageName::ExportTextPathToObjectPath(Tag) != Groom->GetPathName()) continue;
		UObject* Binding = Data.GetAsset();
		if (ObjectProperty(Binding,TEXT("Groom")) != Groom) continue;
		if (HasAssociation) *HasAssociation = true;
		USkeletalMesh* SourceHead = Cast<USkeletalMesh>(ObjectProperty(Binding,TEXT("SourceSkeletalMesh")));
		USkeletalMesh* TargetHead = Cast<USkeletalMesh>(ObjectProperty(Binding,TEXT("TargetSkeletalMesh")));
		if ((SourceHead && !IsHead(SourceHead)) || (TargetHead && !IsHead(TargetHead)))
		{ Error = TEXT("the Groom binding does not identify MetaHuman facial authoring meshes"); return nullptr; }
		Evidence.Add({Groom->GetPathName(),SourceHead ? SourceHead->GetPathName() : FString(),TargetHead ? TargetHead->GetPathName() : FString()});
	}
	FString HeadPath;
	return ChooseHead(Groom->GetPathName(),Evidence,HeadPath,Error) ? LoadObject<USkeletalMesh>(nullptr,*HeadPath) : nullptr;
}
