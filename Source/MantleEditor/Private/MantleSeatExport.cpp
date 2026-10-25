// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSeatExport.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceValidator.h"
#include "MantleSeatSpot.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSeatExportPrivate
{
	FString SeatLabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** How far past the highest hip the floor trace still looks, so "too high" is told apart from "no floor". */
	constexpr double FloorTraceMargin = 400.0;

	/** To a tenth, so a seat does not write 44.999996. */
	double SeatRounded(double Value)
	{
		return FMath::RoundToDouble(Value * 10.0) / 10.0;
	}
}

void FMantleSeatExport::Collect(const AActor* Actor, TArray<const UMantleSeatSpotComponent*>& Out)
{
	TArray<UMantleSeatSpotComponent*> Found;
	Actor->GetComponents<UMantleSeatSpotComponent>(Found);
	Found.Sort([](const UMantleSeatSpotComponent& A, const UMantleSeatSpotComponent& B) { return A.GetName() < B.GetName(); });
	for (const UMantleSeatSpotComponent* Seat : Found)
	{
		Out.Add(Seat);
	}
}

bool FMantleSeatExport::OwnerPublishes(const AActor* Actor)
{
	if (!Actor || FMantlePlaceExporter::IsProvidedByMantle(Actor) || FMantlePlaceExporter::IsEngineInfrastructure(Actor))
	{
		return false;
	}
	FString What;
	FString Remedy;
	if (FMantlePlaceExporter::DescribeUnpublishedContent(Actor, What, Remedy))
	{
		return false;
	}
	EMantleEntityType Type;
	return FMantlePlaceExporter::ClassifyActor(Actor, Type);
}

FString FMantleSeatExport::OwnerProblem(const AActor* Actor)
{
	TArray<UPrimitiveComponent*> Primitives;
	Actor->GetComponents<UPrimitiveComponent>(Primitives);
	for (const UPrimitiveComponent* Primitive : Primitives)
	{
		if (Primitive->BodyInstance.bSimulatePhysics)
		{
			return TEXT("the prop simulates physics, so the seat would move.");
		}
	}
	// Movable as Export reads it (its bDynamic): a prefab only when something could move it, since a new Blueprint's
	// root is Movable by default; any other actor when its root is Movable.
	TArray<FMantlePrefabPart> Parts;
	FMantlePlaceExporter::CollectPrefabParts(Actor, Parts);
	const USceneComponent* Root = Actor->GetRootComponent();
	const bool bDynamic = Parts.Num() > 0 ? FMantlePlaceExporter::IsPrefabDynamic(Actor) : (Root && Root->Mobility == EComponentMobility::Movable);
	if (bDynamic)
	{
		return TEXT("the prop is Movable, so the seat could move.");
	}
	return FString();
}

EMantleSeatStyle FMantleSeatExport::StyleOf(const UMantleSeatSpotComponent* Seat)
{
	switch (Seat->Style)
	{
	case EMantleSeatSpotStyle::Bench:
	default:
		return EMantleSeatStyle::Bench;
	}
}

FMantleSeat FMantleSeatExport::ToPlaceSeat(const FString& EntityId, const FTransform& OwnerWorld, const FTransform& SeatWorld, EMantleSeatStyle Style)
{
	using namespace MantleSeatExportPrivate;
	FMantleSeat Seat;
	Seat.Entity = EntityId;
	const FVector Local = OwnerWorld.InverseTransformPosition(SeatWorld.GetLocation());
	Seat.Position = FVector(SeatRounded(Local.X), SeatRounded(Local.Y), SeatRounded(Local.Z));
	// Yaw only: the seat's heading against the owner's, folded into (-180, 180].
	Seat.Yaw = SeatRounded(FRotator::NormalizeAxis(SeatWorld.GetRotation().Rotator().Yaw - OwnerWorld.GetRotation().Rotator().Yaw));
	Seat.Style = Style;
	return Seat;
}

bool FMantleSeatExport::FloorBelow(const UMantleSeatSpotComponent* Seat, double& OutHipHeight)
{
	UWorld* World = Seat->GetWorld();
	if (!World)
	{
		return false;
	}
	const FVector Hip = Seat->GetComponentLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MantleSeatFloor), /*bTraceComplex*/ true, Seat->GetOwner());
	FHitResult Hit;
	const FVector End = Hip - FVector(0.0, 0.0, MaxHipHeight + MantleSeatExportPrivate::FloorTraceMargin);
	if (!World->LineTraceSingleByChannel(Hit, Hip, End, ECC_Visibility, Params))
	{
		return false;
	}
	OutHipHeight = Hip.Z - Hit.ImpactPoint.Z;
	return true;
}

FString FMantleSeatExport::HeightProblem(bool bFound, double HipHeight)
{
	if (!bFound)
	{
		return TEXT("has a seat with nothing to sit on: no floor was found under its hip point.");
	}
	if (HipHeight < MinHipHeight)
	{
		return FString::Printf(TEXT("has a seat whose hip is only %.0f cm above the floor; it needs at least %.0f cm."), HipHeight, MinHipHeight);
	}
	if (HipHeight > MaxHipHeight)
	{
		return FString::Printf(TEXT("has a seat with nothing to sit on: its hip is %.0f cm above the floor, and at most %.0f cm is allowed."), HipHeight, MaxHipHeight);
	}
	return FString();
}

void FMantleSeatExport::Check(UWorld* World, FMantleValidationResult& Result)
{
	using namespace MantleSeatExportPrivate;
	int32 Seats = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}
		TArray<const UMantleSeatSpotComponent*> Components;
		Collect(Actor, Components);
		if (Components.IsEmpty())
		{
			continue;
		}
		Seats += Components.Num();
		const FString Label = SeatLabelOf(Actor);
		if (!OwnerPublishes(Actor))
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error, Label,
				TEXT("has a Mantle Seat, but the prop itself is not published, so the seat has nothing to belong to."),
				TEXT("Make the prop publishable, or move the seat onto a prop that publishes.") });
			continue;
		}
		const FString Problem = OwnerProblem(Actor);
		if (!Problem.IsEmpty())
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error, Label,
				FString::Printf(TEXT("has a Mantle Seat, but %s"), *Problem),
				TEXT("Turn Simulate Physics off and make the prop Static, or move the seat onto a fixed prop.") });
		}
		for (const UMantleSeatSpotComponent* Seat : Components)
		{
			double HipHeight = 0.0;
			const bool bFound = FloorBelow(Seat, HipHeight);
			const FString HeightIssue = HeightProblem(bFound, HipHeight);
			if (!HeightIssue.IsEmpty())
			{
				Result.Issues.Add(FMantleValidationIssue{
					EMantleIssueSeverity::Error, Label, HeightIssue,
					TEXT("Put the seat's hip point about 45 cm above the floor it sits on, with a floor under it.") });
			}
		}
	}
	if (Seats > FMantlePlaceLimits::MaxSeatsPerPlace)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("This level has %d Mantle Seats; a place holds at most %d."), Seats, FMantlePlaceLimits::MaxSeatsPerPlace),
			TEXT("Remove some seats.") });
	}
}

void FMantleSeatExport::Export(const TArray<const AActor*>& Exportable, const TMap<const AActor*, FString>& EntityIdByActor,
	TArray<FMantleSeat>& OutSeats, TArray<FMantleParseError>& OutErrors)
{
	using namespace MantleSeatExportPrivate;
	int32 Count = 0;
	for (const AActor* Actor : Exportable)
	{
		const FString* Id = EntityIdByActor.Find(Actor);
		TArray<const UMantleSeatSpotComponent*> Components;
		Collect(Actor, Components);
		if (!Id || Components.IsEmpty())
		{
			continue;
		}
		for (const UMantleSeatSpotComponent* Seat : Components)
		{
			if (++Count == FMantlePlaceLimits::MaxSeatsPerPlace + 1)
			{
				OutErrors.Add(FMantleParseError{ TEXT("/seats"),
					FString::Printf(TEXT("'%s' holds seat number %d; a place may have at most %d. Remove some seats."),
						*SeatLabelOf(Actor), Count, FMantlePlaceLimits::MaxSeatsPerPlace) });
			}
			if (Count > FMantlePlaceLimits::MaxSeatsPerPlace)
			{
				continue;
			}
			// The entity's frame in the world is the actor's, whether Export writes it relative to a parent or not.
			OutSeats.Add(ToPlaceSeat(*Id, Actor->GetActorTransform(), Seat->GetComponentTransform(), StyleOf(Seat)));
		}
	}
}
