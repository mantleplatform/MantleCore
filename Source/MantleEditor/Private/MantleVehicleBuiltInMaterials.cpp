// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuiltInMaterials.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "MantlePlaceAssets.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleSource.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBuiltInMaterials"

namespace MantleVehicleBuiltInMaterialsPrivate
{
	bool HasAny(const FString& Lower, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}
}

TArray<FString> FMantleVehicleBuiltInMaterials::All()
{
	return { Rubber(), Glass(), Chrome(), LightLens() };
}

bool FMantleVehicleBuiltInMaterials::IsBuiltIn(const FString& Id)
{
	return All().Contains(Id);
}

FString FMantleVehicleBuiltInMaterials::Suggest(const FString& SectionName)
{
	using namespace MantleVehicleBuiltInMaterialsPrivate;
	const FString Lower = SectionName.ToLower();
	// A part that sits next to glass or rubber but is not it.
	if (HasAny(Lower, { TEXT("wiper"), TEXT("frame"), TEXT("seal"), TEXT("gasket"), TEXT("mirror"), TEXT("sticker"), TEXT("tint_film") }))
	{
		return FString();
	}
	if (HasAny(Lower, { TEXT("lens") }))
	{
		return LightLens();
	}
	if (HasAny(Lower, { TEXT("glass"), TEXT("window"), TEXT("windscreen"), TEXT("windshield") }))
	{
		return Glass();
	}
	if (HasAny(Lower, { TEXT("tire"), TEXT("tyre") }) && !HasAny(Lower, { TEXT("rim"), TEXT("hub"), TEXT("alloy"), TEXT("brake"), TEXT("disc"), TEXT("disk") }))
	{
		return Rubber();
	}
	if (HasAny(Lower, { TEXT("chrome") }))
	{
		return Chrome();
	}
	return FString();
}

FString FMantleVehicleBuiltInMaterials::Detect(const UMaterialInterface* Material, const FString& SectionName, const bool bLight)
{
	if (bLight)
	{
		return LightLens();
	}
	FString ByName = Suggest(SectionName);
	if (ByName.IsEmpty() && Material)
	{
		ByName = Suggest(Material->GetName());
	}
	if (!ByName.IsEmpty())
	{
		return ByName;
	}
	if (Material && Material->GetBlendMode() == BLEND_Translucent)
	{
		const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
		if (Analysis.bOpacityFlat && Analysis.FlatOpacity < 0.95 && Analysis.UnpublishableWhy.IsEmpty() && !Analysis.Fidelity2.bVehicleLight)
		{
			return Glass();
		}
	}
	return FString();
}

FString FMantleVehicleBuiltInMaterials::ForSection(const UMantleVehicleSource& Source, const FString& SectionName, const UMaterialInterface* Material)
{
	if (Source.Paint.Sections.ContainsByPredicate([&SectionName](const FString& Painted) { return Painted.Equals(SectionName, ESearchCase::IgnoreCase); })
		|| Source.Paint.Layers.ContainsByPredicate([&SectionName](const FMantleVehicleSourcePaintLayer& Layer)
		{
			return Layer.Sections.ContainsByPredicate([&SectionName](const FString& Painted) { return Painted.Equals(SectionName, ESearchCase::IgnoreCase); });
		}))
	{
		return FString();
	}
	bool bLight = false;
	for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds && !bLight; ++Kind)
	{
		bLight = FMantleVehicleLights::Sections(Source.Lights, static_cast<EMantleLightKind>(Kind)).ContainsByPredicate(
			[&SectionName](const FString& Light) { return Light.Equals(SectionName, ESearchCase::IgnoreCase); });
	}
	return Detect(Material, SectionName, bLight);
}

FString FMantleVehicleBuiltInMaterials::ForPartSlot(const FMantleVehicleSourcePart& Part, const UStaticMesh* Mesh, const int32 Slot)
{
	if (!Mesh || Part.PaintSlots.Contains(Slot)
		|| Part.LayerSlots.ContainsByPredicate([Slot](const FMantleVehicleSourcePartLayerSlots& Layer) { return Layer.Slots.Contains(Slot); }))
	{
		return FString();
	}
	const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
	const FString Name = Slots.IsValidIndex(Slot) ? Slots[Slot].MaterialSlotName.ToString() : FString();
	return Detect(FMantleVehicleConverter::PartSlotMaterial(Part, Mesh, Slot), Name, /*bLight*/ false);
}

FString FMantleVehicleBuiltInMaterials::Describe(const UMantleVehicleSource& Source)
{
	TMap<FString, TArray<FString>> ByMaterial;
	if (const USkeletalMesh* Body = Source.Body.LoadSynchronous())
	{
		const TArray<FString> Sections = FMantleVehicleLights::SectionNames(Body->GetMaterials());
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			const FString Id = ForSection(Source, Sections[Index], Body->GetMaterials()[Index].MaterialInterface);
			if (!Id.IsEmpty())
			{
				ByMaterial.FindOrAdd(Id).AddUnique(Sections[Index]);
			}
		}
	}
	for (const FMantleVehicleSourcePart& Part : Source.Parts)
	{
		const UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		for (int32 Slot = 0; Mesh && Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
		{
			const FString Id = ForPartSlot(Part, Mesh, Slot);
			if (!Id.IsEmpty())
			{
				ByMaterial.FindOrAdd(Id).AddUnique(Mesh->GetStaticMaterials()[Slot].MaterialSlotName.ToString());
			}
		}
	}
	TArray<FString> Lines;
	for (const FString& Id : All())
	{
		if (const TArray<FString>* Sections = ByMaterial.Find(Id))
		{
			Lines.Add(FString::Printf(TEXT("%s for %s"), *Label(Id).ToString(), *FString::Join(*Sections, TEXT(", "))));
		}
	}
	return Lines.IsEmpty() ? FString()
		: FString::Printf(TEXT("Mantle's own materials are used where your model has them, so nothing of yours uploads for them: %s. Every other part keeps your material."),
			*FString::Join(Lines, TEXT("; ")));
}

FText FMantleVehicleBuiltInMaterials::Label(const FString& Id)
{
	if (Id == Rubber())
	{
		return LOCTEXT("Rubber", "Tire rubber");
	}
	if (Id == Glass())
	{
		return LOCTEXT("Glass", "Glass");
	}
	if (Id == Chrome())
	{
		return LOCTEXT("Chrome", "Chrome");
	}
	if (Id == LightLens())
	{
		return LOCTEXT("LightLens", "Light lens");
	}
	return FText::FromString(Id);
}

// Mantle.Vehicle.BuiltInMaterials <vehicle asset>: what the converter gives Mantle's own materials in a vehicle, section by section and slot by slot, in the log.
namespace MantleVehicleBuiltInMaterialsPrivate
{
	void ListBuiltIns(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogTemp, Warning, TEXT("MANTLE_BUILTIN usage: Mantle.Vehicle.BuiltInMaterials <vehicle asset path>"));
			return;
		}
		const UMantleVehicleSource* Source = LoadObject<UMantleVehicleSource>(nullptr, *Args[0]);
		if (!Source)
		{
			// A model that has no vehicle yet: its slots by name and material alone (the lights and paint are a built vehicle's).
			if (const USkeletalMesh* Model = LoadObject<USkeletalMesh>(nullptr, *Args[0]))
			{
				int32 ModelReplaced = 0;
				const TArray<FString> Sections = FMantleVehicleLights::SectionNames(Model->GetMaterials());
				for (int32 Index = 0; Index < Sections.Num(); ++Index)
				{
					const UMaterialInterface* Material = Model->GetMaterials()[Index].MaterialInterface;
					const FString Id = FMantleVehicleBuiltInMaterials::Detect(Material, Sections[Index], /*bLight*/ false);
					UE_LOG(LogTemp, Display, TEXT("MANTLE_BUILTIN model %s section '%s' (%s) -> %s"), *Model->GetName(), *Sections[Index], Material ? *Material->GetName() : TEXT("no material"),
						Id.IsEmpty() ? TEXT("its own material") : *Id);
					ModelReplaced += Id.IsEmpty() ? 0 : 1;
				}
				UE_LOG(LogTemp, Display, TEXT("MANTLE_BUILTIN %s: %d of %d sections replaced by Mantle's materials"), *Model->GetName(), ModelReplaced, Sections.Num());
				return;
			}
			UE_LOG(LogTemp, Warning, TEXT("MANTLE_BUILTIN %s is not a Mantle vehicle asset or a skeletal mesh"), *Args[0]);
			return;
		}
		int32 Replaced = 0;
		int32 Kept = 0;
		if (const USkeletalMesh* Body = Source->Body.LoadSynchronous())
		{
			const TArray<FString> Sections = FMantleVehicleLights::SectionNames(Body->GetMaterials());
			for (int32 Index = 0; Index < Sections.Num(); ++Index)
			{
				const UMaterialInterface* Material = Body->GetMaterials()[Index].MaterialInterface;
				const FString Id = FMantleVehicleBuiltInMaterials::ForSection(*Source, Sections[Index], Material);
				UE_LOG(LogTemp, Display, TEXT("MANTLE_BUILTIN body section '%s' (%s) -> %s"), *Sections[Index], Material ? *Material->GetName() : TEXT("no material"),
					Id.IsEmpty() ? TEXT("its own material") : *Id);
				(Id.IsEmpty() ? Kept : Replaced)++;
			}
		}
		for (int32 PartIndex = 0; PartIndex < Source->Parts.Num(); ++PartIndex)
		{
			const FMantleVehicleSourcePart& Part = Source->Parts[PartIndex];
			const UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
			for (int32 Slot = 0; Mesh && Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
			{
				const UMaterialInterface* Material = FMantleVehicleConverter::PartSlotMaterial(Part, Mesh, Slot);
				const FString Id = FMantleVehicleBuiltInMaterials::ForPartSlot(Part, Mesh, Slot);
				UE_LOG(LogTemp, Display, TEXT("MANTLE_BUILTIN part %d slot %d '%s' (%s) -> %s"), PartIndex, Slot, *Mesh->GetStaticMaterials()[Slot].MaterialSlotName.ToString(),
					Material ? *Material->GetName() : TEXT("no material"), Id.IsEmpty() ? TEXT("its own material") : *Id);
				(Id.IsEmpty() ? Kept : Replaced)++;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("MANTLE_BUILTIN %s: %d replaced by Mantle's materials, %d keep their own"), *Source->GetName(), Replaced, Kept);
	}

	FAutoConsoleCommand ListBuiltInsCommand(TEXT("Mantle.Vehicle.BuiltInMaterials"),
		TEXT("Lists which sections and slots of a Mantle vehicle asset get Mantle's own glass, rubber, chrome or light lens when it is converted."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ListBuiltIns));
}

#undef LOCTEXT_NAMESPACE
