// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairGeneration.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleHairSourceDiscovery.h"
#include "Engine/StaticMesh.h"
#include "GroomAsset.h"
#include "GroomAssetCards.h"
#include "HairDescription.h"
#include "IHairCardGenerator.h"
#include "Features/IModularFeatures.h"
#include "UObject/UnrealType.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace MantleHairGenerationPrivate
{
	bool SetNumber(UObject* Object, const TCHAR* Name, int64 Value)
	{
		if (!Object) return false;
		FProperty* P = FindFProperty<FProperty>(Object->GetClass(),Name);
		FNumericProperty* Numeric = CastField<FNumericProperty>(P);
		if (const auto* Enum = CastField<FEnumProperty>(P)) Numeric = Enum->GetUnderlyingProperty();
		if (!Numeric) return false;
		Numeric->SetIntPropertyValue(P->ContainerPtrToValuePtr<void>(Object), Value);
		return Numeric->GetSignedIntPropertyValue(P->ContainerPtrToValuePtr<void>(Object)) == Value;
	}
	UHairCardGenerationSettings* BudgetSettings(UGroomAsset* Groom, int32 TriangleCap)
	{
		// The concrete implementation is optional; use its reflected settings without linking its disabled module.
		UClass* Class = FindObject<UClass>(nullptr,TEXT("/Script/HairCardGeneratorEditor.HairCardGeneratorPluginSettings"));
		UClass* GroupClass = FindObject<UClass>(nullptr,TEXT("/Script/HairCardGeneratorEditor.HairCardGeneratorGroupSettings"));
		if (!Class || !GroupClass || !Class->IsChildOf(UHairCardGenerationSettings::StaticClass())) return nullptr;
		auto* Settings = NewObject<UHairCardGenerationSettings>(Groom,Class,NAME_None,RF_Transient);
		if (!SetNumber(Settings,TEXT("AtlasSize"),11) // Epic EHairCardAtlasSize::AtlasSize2048.
			|| !SetNumber(Settings,TEXT("ChannelLayout"),2) || !SetNumber(Settings,TEXT("RandomSeed"),647)) return nullptr;
		if (auto* All = FindFProperty<FBoolProperty>(Class,TEXT("bGenerateGeometryForAllGroups"))) All->SetPropertyValue_InContainer(Settings,true);
		bool bGroupConfigured = false;
		if (auto* Groups = FindFProperty<FArrayProperty>(Class,TEXT("FilterGroupGenerationSettings")))
		{
			if (auto* Object = CastField<FObjectPropertyBase>(Groups->Inner))
			{
				FScriptArrayHelper Array(Groups,Groups->ContainerPtrToValuePtr<void>(Settings)); Array.EmptyValues();
				UObject* Group = NewObject<UObject>(Settings,GroupClass,NAME_None,RF_Transient);
				if (!SetNumber(Group,TEXT("TargetNumberOfCards"),500) || !SetNumber(Group,TEXT("TargetTriangleCount"),TriangleCap)
					|| !SetNumber(Group,TEXT("MaxNumberOfFlyaways"),0) || !SetNumber(Group,TEXT("NumberOfTexturesInAtlas"),16)) return nullptr;
				// Card-group names are strand filters, distinct from physical Groom groups.
				// Include every authored name in this single bounded settings group.
				auto* Filters = FindFProperty<FSetProperty>(GroupClass,TEXT("ApplyToCardGroups"));
				auto* Name = Filters ? CastField<FNameProperty>(Filters->ElementProp) : nullptr;
				if (!Name) return nullptr;
				TSet<FName> CardNames{NAME_None};
				const FHairDescription Description = Groom->GetHairDescription();
				const auto Names = Description.StrandAttributes().GetAttributesRef<FName>(HairAttribute::Strand::GroupCardsName);
				if (Names.IsValid()) for (int32 Strand = 0; Strand < Description.GetNumStrands(); ++Strand) CardNames.Add(Names[FStrandID(Strand)]);
				FScriptSetHelper FilterSet(Filters,Filters->ContainerPtrToValuePtr<void>(Group)); FilterSet.EmptyElements();
				for (FName CardName : CardNames) Name->SetPropertyValue(FilterSet.GetElementPtr(FilterSet.AddDefaultValue_Invalid_NeedsRehash()),CardName);
				FilterSet.Rehash();
				Object->SetObjectPropertyValue(Array.GetRawPtr(Array.AddValue()),Group);
				bGroupConfigured = true;
			}
		}
		return bGroupConfigured ? Settings : nullptr;
	}
}

bool FMantleHairGeneration::CanGenerate(const UObject* Groom, FString& Why)
{
	Why.Reset();
	if (!FMantleCosmeticExport::IsGroomAsset(Groom)) { Why = TEXT("select a strand Groom asset to generate cards"); return false; }
	if (IsRunningCommandlet()) { Why = TEXT("native hair-card generation uses the interactive editor; generate cards there, then export them"); return false; }
	if (!IModularFeatures::Get().IsModularFeatureAvailable(IHairCardGenerator::ModularFeatureName))
	{ Why = TEXT("the optional Hair Card Generator is unavailable: enable it in this creator project and complete its Python setup, or use MetaHuman Optimized build cards; Mantle does not enable plugins or install dependencies"); return false; }
	return true;
}

bool FMantleHairGeneration::Generate(UObject* Object, FString& Error, const FString& Slot)
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&Error);
	if (!Contract) return false;
	if (!Contract->HairSlots.Contains(Slot)) { Error = TEXT("choose a hair slot before generating cards"); return false; }
	if (!CanGenerate(Object,Error)) return false;
	UGroomAsset* Groom = Cast<UGroomAsset>(Object);
	if (!Groom || Groom->GetHairGroupsInfo().IsEmpty()) { Error = TEXT("the groom has no strand groups to generate"); return false; }
	// LOD 0 exists even for a groom that has no authored coarse strand LODs.
	FHairGroupsCardsSourceDescription Cards; Cards.GroupIndex = 0; Cards.LODIndex = 0; Cards.Textures.Layout = EHairTextureLayout::Layout2;
	Cards.GenerationSettings = MantleHairGenerationPrivate::BudgetSettings(Groom, Contract->SkinnedMaxTriangles(Slot));
	if (!Cards.GenerationSettings) { Error = TEXT("the installed generator exposes no compatible budget settings; generate cards in the Groom editor and import its completed mesh/atlases"); return false; }
	const auto Original = Groom->GetHairGroupsCards();
	auto& Generator = IModularFeatures::Get().GetModularFeature<IHairCardGenerator>(IHairCardGenerator::ModularFeatureName);
	if (!Generator.IsCompatibleSettings(Cards.GenerationSettings))
	{ Error = TEXT("the active hair-card generator does not accept these generation settings; use its Groom editor action"); return false; }
	if (!Generator.GenerateHairCardsForLOD(Groom,Cards) || !Cards.ImportedMesh)
	{ Groom->SetHairGroupsCards(Original); Error = TEXT("native card generation was cancelled or produced no mesh; check its editor/Python diagnostics, or use existing optimized cards"); return false; }
	Groom->Modify(); auto Updated = Original;
	Updated.RemoveAll([&Cards](const auto& Existing) { return Existing.GroupIndex == Cards.GroupIndex && Existing.LODIndex == Cards.LODIndex; });
	Updated.Add(Cards); Groom->SetHairGroupsCards(Updated); Groom->MarkPackageDirty();
	const FString OriginalGroom = Groom->GetOutermost()->GetMetaData().GetValue(Groom,FMantleHairSourceDiscovery::OriginalGroomTag);
	Cards.ImportedMesh->GetOutermost()->GetMetaData().SetValue(Cards.ImportedMesh,FMantleHairSourceDiscovery::OriginalGroomTag,
		*(OriginalGroom.IsEmpty() ? Groom->GetPathName() : OriginalGroom));
	Cards.ImportedMesh->MarkPackageDirty();
	return true;
}
