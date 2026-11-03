// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWheelPublish.h"

#include "MantleApiClient.h"
#include "MantleAttribution.h"
#include "MantleCreatorSignIn.h"
#include "MantlePlaceAssets.h"
#include "MantleProvenance.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"
#include "MantleVehicleConverter.h"
#include "MantleWheelDocument.h"

#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialDomain.h"
#include "Misc/Paths.h"

// =============================================================================================
// One wheel, as a publish source

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWheelPublishPrivate
{
	/** The material a wheel's upload slot draws with: the mesh's own, else Unreal's default surface (a wheel names no library material). */
	UMaterialInterface* SlotMaterial(const UStaticMesh* Mesh, int32 Slot)
	{
		UMaterialInterface* Material = FMantlePlaceAssets::MeshSlotMaterial(Mesh, Slot);
		return Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
	}

	/** What the slot chooser reads about each upload slot of Mesh, in upload order. */
	TArray<FMantleWheelSlotFacts> SlotFacts(const UStaticMesh* Mesh)
	{
		TArray<FMantleWheelSlotFacts> Out;
		const TArray<FStaticMaterial>& Statics = Mesh->GetStaticMaterials();
		for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
		{
			const UMaterialInterface* Material = SlotMaterial(Mesh, Slot);
			FMantleWheelSlotFacts& Facts = Out.AddDefaulted_GetRef();
			Facts.SlotName = Statics.IsValidIndex(Slot) ? Statics[Slot].MaterialSlotName.ToString() : FString();
			Facts.MaterialName = Material ? Material->GetName() : FString();
			Facts.bPaint = FMantleVehicleConverter::LooksLikePaint(Material);
		}
		return Out;
	}

	const TCHAR* const PlaceholderId = TEXT("ast_u00000000000000000000000000000000");
}

FMantleWheelPublishSource::FMantleWheelPublishSource(FString InStaticMeshPath, FString InWheelName, TSharedPtr<FMantlePlaceAssetConverter> InConverter)
	: StaticMeshPath(MoveTemp(InStaticMeshPath))
	, WheelName(InWheelName.TrimStartAndEnd())
	, Converter(MoveTemp(InConverter))
{
}

FString FMantleWheelPublishSource::LibraryPath(const FString& Name)
{
	return TEXT("/Game/MantleWheels/") + Name;
}

FString FMantleWheelPublishSource::WheelKey(const FString& Name)
{
	return TEXT("wheel:") + LibraryPath(Name);
}

UStaticMesh* FMantleWheelPublishSource::LoadWheelMesh(const FString& StaticMeshPath)
{
	FString ObjectPath = StaticMeshPath.TrimStartAndEnd();
	if (!ObjectPath.StartsWith(TEXT("/Game/")))
	{
		return nullptr;
	}
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath += TEXT(".") + FPaths::GetCleanFilename(ObjectPath);
	}
	return LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

FString FMantleWheelPublishSource::ProblemWith(const FString& StaticMeshPath, const FString& Name, FMantleWheelItemMeasure* OutMeasure)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return TEXT("Give the wheel a name.");
	}
	if (Trimmed.Len() > FMantleWheelDocument::MaxNameLength)
	{
		return FString::Printf(TEXT("The wheel's name is %d characters long; a wheel name can be at most %d."), Trimmed.Len(), FMantleWheelDocument::MaxNameLength);
	}
	const UStaticMesh* Mesh = LoadWheelMesh(StaticMeshPath);
	if (!Mesh)
	{
		return FString::Printf(TEXT("'%s' is not a static mesh in your project. Give the path of the wheel's model, such as /Game/Wheels/SM_MyWheel."), *StaticMeshPath);
	}
	const FMantleWheelItemMeasure Measure = FMantleWheelItem::MeasureMesh(*Mesh);
	if (OutMeasure)
	{
		*OutMeasure = Measure;
	}
	if (!Measure.bOk)
	{
		return FString::Printf(TEXT("The wheel '%s' cannot be published: %s"), *Trimmed, *Measure.Problem);
	}
	const int32 NumSlots = FMantlePlaceAssets::UploadSlots(Mesh).Num();
	if (NumSlots == 0)
	{
		return FString::Printf(TEXT("The wheel '%s' has no material slots with geometry."), *Trimmed);
	}
	// The document's own rules (name, sizes, slot count): the same ones the upload is held to, said before anything is sent.
	TArray<FString> Ids;
	Ids.Init(MantleWheelPublishPrivate::PlaceholderId, NumSlots);
	FString Json;
	FString Error;
	if (!FMantleWheelDocument::Build(Trimmed, MantleWheelPublishPrivate::PlaceholderId, Ids, Measure.ReferenceRadius, Measure.ReferenceWidth, TArray<int32>(), Json, Error))
	{
		return Error;
	}
	return FString();
}

bool FMantleWheelPublishSource::Prepare(FMantleValidationResult& OutValidation, FString& OutMessage)
{
	OutValidation = FMantleValidationResult();
	FMantleWheelItemMeasure Measure;
	const FString Problem = ProblemWith(StaticMeshPath, WheelName, &Measure);
	if (!Problem.IsEmpty())
	{
		OutMessage = Problem;
		return false;
	}
	// The item is remembered per API and account, as a place's vehicles and weapons are.
	LinkApi = FMantleApiClient::BaseUrlResolver();
	FString WhyNot;
	if (!FMantleCreatorSignIn::ResolveAccountId(LinkAccount, WhyNot))
	{
		OutMessage = WhyNot + TEXT(" Sign in again (Tools > Mantle) and choose Publish again; nothing was uploaded.");
		return false;
	}
	UStaticMesh* Mesh = LoadWheelMesh(StaticMeshPath);
	if (!Mesh)
	{
		OutMessage = FString::Printf(TEXT("The wheel's model '%s' could not be loaded."), *StaticMeshPath);
		return false;
	}
	// The model uploads as any mesh does, then the material of each upload slot, always uploaded (a library material is not named by a wheel).
	Dependencies = FMantlePlaceDependencies();
	Dependencies.AddMesh(Mesh, WheelName);
	Converted = FMantleConvertedWheel();
	Converted.Key = WheelKey(WheelName);
	Converted.Name = WheelName;
	Converted.Path = LibraryPath(WheelName);
	Converted.ImportFilename = FMantleProvenance::ImportFilenameOf(Mesh);
	Converted.MeshKey = FMantlePlaceAssets::MeshKey(Mesh);
	Converted.ReferenceRadius = Measure.ReferenceRadius;
	Converted.ReferenceWidth = Measure.ReferenceWidth;
	for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
	{
		UMaterialInterface* Material = MantleWheelPublishPrivate::SlotMaterial(Mesh, Slot);
		const FString MaterialKey = FMantlePlaceAssets::MaterialKey(Material);
		Converted.MaterialKeys.Add(MaterialKey);
		FMantleGatheredMaterial* Existing = Dependencies.Materials.FindByPredicate([&MaterialKey](const FMantleGatheredMaterial& Gathered) { return Gathered.Key == MaterialKey; });
		if (!Existing)
		{
			Existing = &Dependencies.Materials.AddDefaulted_GetRef();
			Existing->Material = Material;
			Existing->Key = MaterialKey;
			Existing->Name = Material->GetName();
		}
		Existing->UsedBy.AddUnique(WheelName);
	}
	Converted.WheelSlots = FMantleWheelDocument::ChooseWheelSlots(MantleWheelPublishPrivate::SlotFacts(Mesh));
	// The gathered model and materials are held only weakly; keep them until the conversion is done.
	Pins.Pin(Dependencies);
	if (!Converter.IsValid())
	{
		Converter = MakeShared<FMantlePlaceAssetConverter>();
	}
	return true;
}

int32 FMantleWheelPublishSource::NumConversionSteps() const
{
	// The mesh and its materials, then one step that records the wheel itself.
	return FMantlePlaceAssetConverter::NumDependencySteps(Dependencies) + 1;
}

FString FMantleWheelPublishSource::ConversionStepName(int32 Step) const
{
	return Step >= FMantlePlaceAssetConverter::NumDependencySteps(Dependencies) ? WheelName : FMantlePlaceAssetConverter::DependencyStepName(Dependencies, Step);
}

void FMantleWheelPublishSource::Convert(int32 Step, FMantlePlaceAssetSet& Set)
{
	if (Step >= FMantlePlaceAssetConverter::NumDependencySteps(Dependencies))
	{
		if (!Set.FindWheel(Converted.Key))
		{
			Set.Wheels.Add(Converted);
		}
		return;
	}
	if (Converter.IsValid())
	{
		Converter->ConvertDependencyStep(Dependencies, Step, Set);
	}
}

bool FMantleWheelPublishSource::CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage)
{
	OutScripts.Reset();
	return true;
}

EMantlePlaceExportOutcome FMantleWheelPublishSource::Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
	const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues)
{
	// A library-only source writes no place file; the session never asks.
	return EMantlePlaceExportOutcome::Failed;
}

FString FMantleWheelPublishSource::FindLibraryItem(const FString& SourcePath)
{
	return UMantleSettings::Get()->FindLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath);
}

void FMantleWheelPublishSource::RememberLibraryItem(const FString& SourcePath, const FString& ItemId)
{
	UMantleSettings::GetMutable()->RememberLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath, ItemId);
}

void FMantleWheelPublishSource::ForgetLibraryItem(const FString& SourcePath)
{
	UMantleSettings::GetMutable()->ForgetLibraryItemForVehicle(LinkApi, LinkAccount, SourcePath);
}

int32 FMantleWheelPublishSource::LibraryGeneration(const FString& SourcePath)
{
	return UMantleSettings::Get()->FindLibraryGenerationForVehicle(LinkApi, LinkAccount, SourcePath);
}

FString FMantleWheelPublishSource::AttributionFor(const FString& ObjectPath) const
{
	return FMantleAttribution::ForObjectPath(ObjectPath);
}

// =============================================================================================
// The wheel Publish

FMantleWheelPublish& FMantleWheelPublish::Get()
{
	static FMantleWheelPublish Instance;
	return Instance;
}

bool FMantleWheelPublish::Start(const FString& StaticMeshPath, const FString& Name, const FMantleVehicleLibraryChoice& Choice, FString& OutError)
{
	if (IsBusy())
	{
		OutError = FString::Printf(TEXT("'%s' is still publishing. Wait for it to finish, then try again."), *WheelName);
		return false;
	}
	if (!Choice.IsReady())
	{
		OutError = TEXT("A public wheel needs the rights declaration: tick it, or choose Private.");
		return false;
	}
	OutError = FMantleWheelPublishSource::ProblemWith(StaticMeshPath, Name);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	WheelName = Name.TrimStartAndEnd();
	const TSharedRef<FMantleWheelPublishSource> Source = MakeShared<FMantleWheelPublishSource>(StaticMeshPath, WheelName);
	// A go-live: the uploads wait for Mantle's approval, which a library item needs.
	Session = MakeShared<FMantlePlacePublishSession>(Source, MakeShared<FMantleApiUploadTransport>(), /*bGoLive*/ true, FString());
	Session->VehicleChoice = Choice;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_WHEEL_PUBLISH started wheel=%s visibility=%s"), *WheelName, Choice.bPublic ? TEXT("public") : TEXT("private"));
	Session->Start();
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMantleWheelPublish::Tick));
	}
	return true;
}

bool FMantleWheelPublish::Tick(float DeltaSeconds)
{
	if (Session.IsValid() && !Session->IsFinished())
	{
		Session->Tick();
	}
	if (!Session.IsValid() || !Session->IsFinished())
	{
		return true;
	}
	if (Session->Succeeded())
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_WHEEL_PUBLISH_DONE wheel=%s %s"), *WheelName, *Session->GetResult().Message);
	}
	else
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_WHEEL_PUBLISH_FAILED wheel=%s %s"), *WheelName, *Session->GetResult().Message);
		for (const FMantleValidationIssue& Issue : Session->GetResult().Validation.Issues)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_WHEEL_PUBLISH_ISSUE %s %s"), *Issue.Problem, *Issue.Remedy);
		}
		for (const FMantleValidationIssue& Issue : Session->GetAttention())
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_WHEEL_PUBLISH_ISSUE %s %s"), *Issue.Problem, *Issue.Remedy);
		}
	}
	TickerHandle.Reset();
	return false;
}

namespace MantleWheelPublishPrivate
{
	// The wheel Publish without a panel, for a harness: the same FMantleWheelPublish.
	FAutoConsoleCommand GMantleWheelPublishCommand(
		TEXT("Mantle.Wheel.Publish"),
		TEXT("Mantle.Wheel.Publish <StaticMeshAssetPath> <Name> [public|private] -- publishes the static mesh (axle along +X, face toward +X) as a ")
		TEXT("library item of kind wheel called Name. private is the default; public ticks the rights declaration on the creator's behalf. Logs ")
		TEXT("MANTLE_WHEEL_PUBLISH_DONE or MANTLE_WHEEL_PUBLISH_FAILED, and MANTLE_LIBRARY_PUBLISHED."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2 || Args.Num() > 3 || (Args.Num() == 3 && !(Args[2] == TEXT("public") || Args[2] == TEXT("private"))))
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_WHEEL_PUBLISH_FAILED usage: Mantle.Wheel.Publish <StaticMeshAssetPath> <Name> [public|private]"));
				return;
			}
			FMantleVehicleLibraryChoice Choice;
			Choice.bPublic = Args.Num() == 3 && Args[2] == TEXT("public");
			Choice.bRightsDeclared = Choice.bPublic;
			FString Error;
			if (!FMantleWheelPublish::Get().Start(Args[0], Args[1], Choice, Error))
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_WHEEL_PUBLISH_FAILED wheel=%s %s"), *Args[1], *Error);
			}
		}));
}
