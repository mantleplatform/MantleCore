// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCrowdSpawner.h"
#include "MantlePlaceLimits.h"

#include "Algo/Sort.h"
#include "Engine/CollisionProfile.h"

// The Details panel's ClampMin/ClampMax metas are literals; these keep them on the place limits.
static_assert(FMantlePlaceLimits::MaxCrowdCount == 100, "update AMantleCrowdSpawner::Count's ClampMax meta");
static_assert(FMantlePlaceLimits::MaxCrowdLaneWidth == 2000.0, "update UMantleCrowdLaneComponent::Width's ClampMax meta");
static_assert(FMantlePlaceLimits::MinCrowdHeightPct == 95 && FMantlePlaceLimits::MaxCrowdHeightPct == 105,
	"update the height metas on AMantleCrowdSpawner and FMantleCrowdLookSettings");
static_assert(FMantlePlaceLimits::MinCrowdWalkSpeed == 50.0 && FMantlePlaceLimits::MaxCrowdWalkSpeed == 300.0,
	"update AMantleCrowdSpawner's walk speed metas");
static_assert(FMantlePlaceLimits::MinCrowdPointLingerSeconds == 1.0 && FMantlePlaceLimits::MaxCrowdPointLingerSeconds == 120.0
	&& FMantlePlaceLimits::MaxCrowdPointRadius == 1000.0, "update FMantleCrowdPointOfInterestSettings' linger and radius metas");

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCrowdSpawnerPrivate
{
	/** Avatar descriptor v2's wear- and accessory-group slots, the parser's and the schema's list. */
	const TCHAR* const CrowdWornSlots[] = {
		TEXT("hair"), TEXT("eyebrows"), TEXT("eyelashes"), TEXT("facial_hair"), TEXT("top"), TEXT("bottom"),
		TEXT("footwear"), TEXT("acc_hat"), TEXT("acc_face"), TEXT("acc_neck"),
		TEXT("acc_shoulder_l"), TEXT("acc_shoulder_r"), TEXT("acc_front"), TEXT("acc_back"), TEXT("acc_waist")
	};

	bool IsCrowdWornSlotName(const FString& Slot)
	{
		for (const TCHAR* Known : CrowdWornSlots)
		{
			if (Slot.Equals(Known, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	FString CrowdWornSlotList()
	{
		TArray<FString> Names;
		for (const TCHAR* Known : CrowdWornSlots)
		{
			Names.Add(Known);
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** Prefix followed by exactly Digits characters, each a digit or, when bHex, a-f. The parser's rule. */
	bool IsCrowdPrefixedId(const FString& Value, const TCHAR* Prefix, int32 Digits, bool bHex)
	{
		const int32 PrefixLen = FCString::Strlen(Prefix);
		if (Value.Len() != PrefixLen + Digits || !Value.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 i = PrefixLen; i < Value.Len(); ++i)
		{
			const TCHAR C = Value[i];
			if (!FChar::IsDigit(C) && !(bHex && C >= TEXT('a') && C <= TEXT('f')))
			{
				return false;
			}
		}
		return true;
	}

	bool IsCrowdItemId(const FString& Value) { return IsCrowdPrefixedId(Value, TEXT("itm_u"), 32, true); }
	bool IsCrowdSkinToneId(const FString& Value) { return IsCrowdPrefixedId(Value, TEXT("tone_"), 2, false); }
	bool IsCrowdEyeColorId(const FString& Value) { return IsCrowdPrefixedId(Value, TEXT("eye_"), 2, false); }

	/** Inside [Min, Max]; written so a NaN is outside. */
	bool IsCrowdInRange(double Value, double Min, double Max)
	{
		return Value >= Min && Value <= Max;
	}

	/** Stable across runs and machines: the plain name, then its number, so Lane_2 comes before Lane_10. */
	bool CrowdLaneNameLess(const FName A, const FName B)
	{
		const int32 Plain = A.GetPlainNameString().Compare(B.GetPlainNameString(), ESearchCase::CaseSensitive);
		return Plain != 0 ? Plain < 0 : A.GetNumber() < B.GetNumber();
	}

	/**
	 * Lane space to actor space. Composed from the relative transforms up to the root, so a lane
	 * straight under the root (every lane ApplyProps makes) maps exactly, with no round trip
	 * through world space; anything else falls back to the world transforms.
	 */
	FTransform CrowdLaneToActorTransform(const USceneComponent& Lane, const AActor& Owner)
	{
		const USceneComponent* Root = Owner.GetRootComponent();
		FTransform Chain = FTransform::Identity;
		for (const USceneComponent* Component = &Lane; Component; Component = Component->GetAttachParent())
		{
			if (Component == Root)
			{
				return Chain;
			}
			if (Component->IsUsingAbsoluteLocation() || Component->IsUsingAbsoluteRotation() || Component->IsUsingAbsoluteScale())
			{
				break;
			}
			Chain = Chain * Component->GetRelativeTransform();
		}
		return Lane.GetComponentTransform().GetRelativeTransform(Owner.GetActorTransform());
	}

	/**
	 * A lane's points in the owner's local space. Straight lanes give their own points, and a
	 * closed loop repeats its first; a lane with any non-linear point is sampled evenly along its
	 * length, at most LaneSampleSpacing apart unless that would pass MaxCrowdLanePoints.
	 */
	void ReadCrowdLanePoints(const UMantleCrowdLaneComponent& Lane, const AActor& Owner, TArray<FVector>& Out)
	{
		const FTransform ToActor = CrowdLaneToActorTransform(Lane, Owner);
		const bool bIdentity = ToActor.Equals(FTransform::Identity, 0.0);
		auto InActorSpace = [&ToActor, bIdentity](const FVector& Local)
		{
			return bIdentity ? Local : ToActor.TransformPosition(Local);
		};

		const int32 NumPoints = Lane.GetNumberOfSplinePoints();
		bool bCurved = false;
		for (int32 i = 0; i < NumPoints; ++i)
		{
			if (Lane.GetSplinePointType(i) != ESplinePointType::Linear)
			{
				bCurved = true;
				break;
			}
		}

		const bool bLoop = Lane.IsClosedLoop();
		if (!bCurved || NumPoints < 2)
		{
			for (int32 i = 0; i < NumPoints; ++i)
			{
				Out.Add(InActorSpace(Lane.GetLocationAtSplinePoint(i, ESplineCoordinateSpace::Local)));
			}
			if (bLoop && Out.Num() > 0)
			{
				const FVector First = Out[0]; // copied: TArray::Add refuses an element of its own
				Out.Add(First);
			}
			return;
		}

		const double Length = Lane.GetSplineLength();
		const int32 Segments = FMath::Clamp(
			FMath::CeilToInt32(Length / AMantleCrowdSpawner::LaneSampleSpacing), 1, FMantlePlaceLimits::MaxCrowdLanePoints - 1);
		// The ends are the lane's own first and last points, exactly; only the inside is sampled.
		Out.Add(InActorSpace(Lane.GetLocationAtSplinePoint(0, ESplineCoordinateSpace::Local)));
		for (int32 Step = 1; Step < Segments; ++Step)
		{
			const double Distance = Length * static_cast<double>(Step) / static_cast<double>(Segments);
			Out.Add(InActorSpace(Lane.GetLocationAtDistanceAlongSpline(static_cast<float>(Distance), ESplineCoordinateSpace::Local)));
		}
		const FVector Last = bLoop ? FVector(Out[0]) : InActorSpace(Lane.GetLocationAtSplinePoint(NumPoints - 1, ESplineCoordinateSpace::Local));
		Out.Add(Last);
	}
}

UMantleCrowdLaneComponent::UMantleCrowdLaneComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Data for the crowd: nothing walks into it, stands on it or sees it in game.
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	CanCharacterStepUpOn = ECB_No;
	bHiddenInGame = true;
}

AMantleCrowdSpawner::AMantleCrowdSpawner()
{
	PrimaryActorTick.bCanEverTick = false;

	// Place data, built on every machine from the place file; the NPCs are each client's own.
	bReplicates = false;
	SetReplicatingMovement(false);
	SetHidden(true);
	SetActorEnableCollision(false);
	SetCanBeDamaged(false);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	DefaultLane = CreateDefaultSubobject<UMantleCrowdLaneComponent>(TEXT("Lane"));
	DefaultLane->SetupAttachment(Root);
}

TArray<UMantleCrowdLaneComponent*> AMantleCrowdSpawner::GetLanes() const
{
	TArray<UMantleCrowdLaneComponent*> Lanes;
	GetComponents(Lanes);
	Lanes.RemoveAll([](const UMantleCrowdLaneComponent* Lane) { return !IsValid(Lane) || Lane->IsBeingDestroyed(); });
	Algo::Sort(Lanes, [](const UMantleCrowdLaneComponent* A, const UMantleCrowdLaneComponent* B)
	{
		return MantleCrowdSpawnerPrivate::CrowdLaneNameLess(A->GetFName(), B->GetFName());
	});
	return Lanes;
}

bool AMantleCrowdSpawner::ReadProps(FMantleCrowdSpawnerProps& Out, FString& OutProblem) const
{
	using namespace MantleCrowdSpawnerPrivate;

	Out = FMantleCrowdSpawnerProps();
	OutProblem.Reset();
	// The first problem wins: one sentence the creator can act on, like the other readers.
	// Not "Fail": MantleRuntimeMesh.cpp has an anonymous-namespace Fail() in the same unity build.
	auto NoteProblem = [&OutProblem](const FString& Problem)
	{
		if (OutProblem.IsEmpty())
		{
			OutProblem = Problem;
		}
	};

	Out.Count = Count;
	Out.Seed = Seed;
	Out.MinHeightPct = MinHeightPct;
	Out.MaxHeightPct = MaxHeightPct;
	Out.SkinTones = SkinTones;
	Out.MinWalkSpeed = WalkSpeedMin;
	Out.MaxWalkSpeed = WalkSpeedMax;
	Out.IdleChance = IdleChance;
	Out.bEnabled = bEnabled;
	for (const FMantleCrowdLookSettings& Look : Looks)
	{
		FMantleCrowdLook& Written = Out.Looks.AddDefaulted_GetRef();
		Written.AvatarItemId = Look.AvatarItemId;
		Written.SkinToneId = Look.SkinToneId;
		Written.EyeColorId = Look.EyeColorId;
		Written.HeightPct = Look.HeightPct;
		for (const FMantleCrowdWornSettings& Worn : Look.Wearing)
		{
			Written.Wearing.Add(FMantleCrowdWorn{ Worn.Slot, Worn.ItemId });
		}
	}

	const TArray<UMantleCrowdLaneComponent*> Lanes = GetLanes();
	for (const UMantleCrowdLaneComponent* Lane : Lanes)
	{
		FMantleCrowdLane& Written = Out.Lanes.AddDefaulted_GetRef();
		Written.Width = Lane->Width;
		Written.bTwoWay = Lane->bTwoWay;
		ReadCrowdLanePoints(*Lane, *this, Written.Points);
	}

	// The parser's limits, in its order (FMantlePlaceParser's ParseCrowdSpawner).
	if (Lanes.Num() < 1)
	{
		NoteProblem(TEXT("has no lanes. Add a Mantle Crowd Lane component for its NPCs to walk."));
	}
	else if (Lanes.Num() > FMantlePlaceLimits::MaxCrowdLanes)
	{
		NoteProblem(FString::Printf(TEXT("has %d lanes; a crowd spawner may have at most %d. Remove lane components until %d are left."),
			Lanes.Num(), FMantlePlaceLimits::MaxCrowdLanes, FMantlePlaceLimits::MaxCrowdLanes));
	}
	for (int32 i = 0; i < Lanes.Num(); ++i)
	{
		const FString LaneName = Lanes[i]->GetName();
		const FMantleCrowdLane& Lane = Out.Lanes[i];
		if (Lane.Points.Num() < 2)
		{
			NoteProblem(FString::Printf(TEXT("has lane '%s' with %d point(s); a lane needs at least 2. Add a point to it, or remove the lane."),
				*LaneName, Lane.Points.Num()));
		}
		else if (Lane.Points.Num() > FMantlePlaceLimits::MaxCrowdLanePoints)
		{
			NoteProblem(FString::Printf(
				TEXT("has lane '%s' with %d points (a closed loop counts its first point twice); a lane may have at most %d. Remove points from it, or split it into two lanes."),
				*LaneName, Lane.Points.Num(), FMantlePlaceLimits::MaxCrowdLanePoints));
		}
		if (!IsCrowdInRange(Lane.Width, 0.0, FMantlePlaceLimits::MaxCrowdLaneWidth))
		{
			NoteProblem(FString::Printf(TEXT("has lane '%s' %g cm wide; a lane is 0 to %g cm wide. Set its Width in that range."),
				*LaneName, Lane.Width, FMantlePlaceLimits::MaxCrowdLaneWidth));
		}
	}

	if (Count < 1 || Count > FMantlePlaceLimits::MaxCrowdCount)
	{
		NoteProblem(FString::Printf(TEXT("has a Count of %d; a crowd spawner holds 1 to %d NPCs. Set Count in that range."),
			Count, FMantlePlaceLimits::MaxCrowdCount));
	}
	if (Seed < 0)
	{
		NoteProblem(FString::Printf(TEXT("has a Seed of %d. Set Seed to 0 or more."), Seed));
	}

	if (Looks.Num() > FMantlePlaceLimits::MaxCrowdLooks)
	{
		NoteProblem(FString::Printf(TEXT("has %d looks; a crowd spawner may have at most %d. Remove looks until %d are left."),
			Looks.Num(), FMantlePlaceLimits::MaxCrowdLooks, FMantlePlaceLimits::MaxCrowdLooks));
	}
	for (int32 i = 0; i < Looks.Num(); ++i)
	{
		const FMantleCrowdLookSettings& Look = Looks[i];
		if (!IsCrowdItemId(Look.AvatarItemId))
		{
			NoteProblem(FString::Printf(
				TEXT("has Looks[%d] with body item '%s', which is not a catalog item id (itm_u and 32 lowercase hex digits). Copy the body's id from the catalog."),
				i, *Look.AvatarItemId));
		}
		if (!Look.SkinToneId.IsEmpty() && !IsCrowdSkinToneId(Look.SkinToneId))
		{
			NoteProblem(FString::Printf(
				TEXT("has Looks[%d] with skin tone '%s'; a skin tone is tone_ and two digits, like tone_03. Fix it, or leave it empty for the seed to pick."),
				i, *Look.SkinToneId));
		}
		if (!Look.EyeColorId.IsEmpty() && !IsCrowdEyeColorId(Look.EyeColorId))
		{
			NoteProblem(FString::Printf(
				TEXT("has Looks[%d] with eye colour '%s'; an eye colour is eye_ and two digits, like eye_02. Fix it, or leave it empty for the seed to pick."),
				i, *Look.EyeColorId));
		}
		if (Look.HeightPct != 0
			&& (Look.HeightPct < FMantlePlaceLimits::MinCrowdHeightPct || Look.HeightPct > FMantlePlaceLimits::MaxCrowdHeightPct))
		{
			NoteProblem(FString::Printf(
				TEXT("has Looks[%d] with a height of %d%%; heights are %d to %d. Set it in that range, or to 0 for the seed to pick."),
				i, Look.HeightPct, FMantlePlaceLimits::MinCrowdHeightPct, FMantlePlaceLimits::MaxCrowdHeightPct));
		}
		if (Look.Wearing.Num() > FMantlePlaceLimits::MaxCrowdLookWearing)
		{
			NoteProblem(FString::Printf(TEXT("has Looks[%d] wearing %d items; a look wears at most %d. Remove items from it."),
				i, Look.Wearing.Num(), FMantlePlaceLimits::MaxCrowdLookWearing));
		}
		TSet<FString> SlotsUsed;
		for (const FMantleCrowdWornSettings& Worn : Look.Wearing)
		{
			bool bSlotUsed = false;
			SlotsUsed.Add(Worn.Slot, &bSlotUsed);
			if (!IsCrowdWornSlotName(Worn.Slot))
			{
				NoteProblem(FString::Printf(TEXT("has Looks[%d] wearing an item in slot '%s', which is not a wear or accessory slot. Use one of: %s."),
					i, *Worn.Slot, *CrowdWornSlotList()));
			}
			else if (bSlotUsed)
			{
				NoteProblem(FString::Printf(TEXT("has Looks[%d] wearing two items in slot '%s'; a look wears one item per slot. Remove one of them."),
					i, *Worn.Slot));
			}
			if (!IsCrowdItemId(Worn.ItemId))
			{
				NoteProblem(FString::Printf(
					TEXT("has Looks[%d] wearing '%s' in slot '%s', which is not a catalog item id (itm_u and 32 lowercase hex digits). Copy the item's id from the catalog."),
					i, *Worn.ItemId, *Worn.Slot));
			}
		}
	}

	if (MinHeightPct < FMantlePlaceLimits::MinCrowdHeightPct || MaxHeightPct > FMantlePlaceLimits::MaxCrowdHeightPct
		|| MinHeightPct > MaxHeightPct)
	{
		NoteProblem(FString::Printf(
			TEXT("has heights from %d%% to %d%%; heights run from %d to %d, and the minimum can't be above the maximum. Set Min and Max Height Pct in that range."),
			MinHeightPct, MaxHeightPct, FMantlePlaceLimits::MinCrowdHeightPct, FMantlePlaceLimits::MaxCrowdHeightPct));
	}
	if (SkinTones.Num() > FMantlePlaceLimits::MaxCrowdSkinTones)
	{
		NoteProblem(FString::Printf(TEXT("lists %d skin tones; a crowd spawner may list at most %d. Remove tones, or empty the list for every tone."),
			SkinTones.Num(), FMantlePlaceLimits::MaxCrowdSkinTones));
	}
	for (int32 i = 0; i < SkinTones.Num(); ++i)
	{
		if (!IsCrowdSkinToneId(SkinTones[i]))
		{
			NoteProblem(FString::Printf(TEXT("lists skin tone '%s'; a skin tone is tone_ and two digits, like tone_03. Fix it or remove it."),
				*SkinTones[i]));
		}
		else if (SkinTones.Find(SkinTones[i]) != i)
		{
			NoteProblem(FString::Printf(TEXT("lists skin tone '%s' twice. Remove one of them."), *SkinTones[i]));
		}
	}

	if (!IsCrowdInRange(WalkSpeedMin, FMantlePlaceLimits::MinCrowdWalkSpeed, FMantlePlaceLimits::MaxCrowdWalkSpeed)
		|| !IsCrowdInRange(WalkSpeedMax, FMantlePlaceLimits::MinCrowdWalkSpeed, FMantlePlaceLimits::MaxCrowdWalkSpeed)
		|| WalkSpeedMin > WalkSpeedMax)
	{
		NoteProblem(FString::Printf(
			TEXT("walks at %g to %g cm/s; speeds run from %g to %g cm/s, and the minimum can't be above the maximum. Set Walk Speed Min and Max in that range."),
			WalkSpeedMin, WalkSpeedMax, FMantlePlaceLimits::MinCrowdWalkSpeed, FMantlePlaceLimits::MaxCrowdWalkSpeed));
	}
	if (!IsCrowdInRange(IdleChance, 0.0, 1.0))
	{
		NoteProblem(FString::Printf(TEXT("has an Idle Chance of %g. Set it from 0 to 1."), IdleChance));
	}

	// #513: points of interest, in the parser's order of checks.
	if (PointsOfInterest.Num() > FMantlePlaceLimits::MaxCrowdPointsOfInterest)
	{
		NoteProblem(FString::Printf(TEXT("has %d points of interest; a crowd spawner may have at most %d. Remove points until %d are left."),
			PointsOfInterest.Num(), FMantlePlaceLimits::MaxCrowdPointsOfInterest, FMantlePlaceLimits::MaxCrowdPointsOfInterest));
	}
	for (int32 i = 0; i < PointsOfInterest.Num(); ++i)
	{
		const FMantleCrowdPointOfInterestSettings& Point = PointsOfInterest[i];
		FMantleCrowdPointOfInterest& Written = Out.PointsOfInterest.AddDefaulted_GetRef();
		Written.EntityId = Point.EntityId;
		Written.Chance = Point.Chance;
		Written.MinLingerSeconds = Point.LingerSecondsMin;
		Written.MaxLingerSeconds = Point.LingerSecondsMax;
		Written.Radius = Point.Radius;

		const AActor* Target = Point.Actor.Get();
		if (Point.Actor.IsNull() && Point.EntityId.IsEmpty())
		{
			NoteProblem(FString::Printf(TEXT("has PointsOfInterest[%d] with no actor. Pick the actor its NPCs gather round, or remove the point."), i));
		}
		else if (Target == this)
		{
			NoteProblem(FString::Printf(TEXT("names itself as PointsOfInterest[%d]. Pick another actor for its NPCs to gather round."), i));
		}
		if (!IsCrowdInRange(Point.Chance, 0.0, 1.0))
		{
			NoteProblem(FString::Printf(TEXT("has PointsOfInterest[%d] with a Chance of %g. Set it from 0 to 1."), i, Point.Chance));
		}
		if (!IsCrowdInRange(Point.LingerSecondsMin, FMantlePlaceLimits::MinCrowdPointLingerSeconds, FMantlePlaceLimits::MaxCrowdPointLingerSeconds)
			|| !IsCrowdInRange(Point.LingerSecondsMax, FMantlePlaceLimits::MinCrowdPointLingerSeconds, FMantlePlaceLimits::MaxCrowdPointLingerSeconds)
			|| Point.LingerSecondsMin > Point.LingerSecondsMax)
		{
			NoteProblem(FString::Printf(
				TEXT("has PointsOfInterest[%d] lingering %g to %g s; stays run from %g to %g s, and the minimum can't be above the maximum. Set Linger Seconds Min and Max in that range."),
				i, Point.LingerSecondsMin, Point.LingerSecondsMax, FMantlePlaceLimits::MinCrowdPointLingerSeconds, FMantlePlaceLimits::MaxCrowdPointLingerSeconds));
		}
		if (!IsCrowdInRange(Point.Radius, 0.0, FMantlePlaceLimits::MaxCrowdPointRadius))
		{
			NoteProblem(FString::Printf(TEXT("has PointsOfInterest[%d] with a Radius of %g cm. Set it from 0 to %g."),
				i, Point.Radius, FMantlePlaceLimits::MaxCrowdPointRadius));
		}
	}

	return OutProblem.IsEmpty();
}

void AMantleCrowdSpawner::ApplyProps(const FMantleCrowdSpawnerProps& In)
{
	Count = In.Count;
	Seed = In.Seed;
	MinHeightPct = In.MinHeightPct;
	MaxHeightPct = In.MaxHeightPct;
	SkinTones = In.SkinTones;
	WalkSpeedMin = In.MinWalkSpeed;
	WalkSpeedMax = In.MaxWalkSpeed;
	IdleChance = In.IdleChance;
	bEnabled = In.bEnabled;
	Looks.Reset(In.Looks.Num());
	for (const FMantleCrowdLook& Look : In.Looks)
	{
		FMantleCrowdLookSettings& Written = Looks.AddDefaulted_GetRef();
		Written.AvatarItemId = Look.AvatarItemId;
		Written.SkinToneId = Look.SkinToneId;
		Written.EyeColorId = Look.EyeColorId;
		Written.HeightPct = Look.HeightPct;
		for (const FMantleCrowdWorn& Worn : Look.Wearing)
		{
			FMantleCrowdWornSettings& WrittenWorn = Written.Wearing.AddDefaulted_GetRef();
			WrittenWorn.Slot = Worn.Slot;
			WrittenWorn.ItemId = Worn.ItemId;
		}
	}
	// #513: by entity id; no actor is known here, so the export names the same entity again.
	PointsOfInterest.Reset(In.PointsOfInterest.Num());
	for (const FMantleCrowdPointOfInterest& Point : In.PointsOfInterest)
	{
		FMantleCrowdPointOfInterestSettings& Written = PointsOfInterest.AddDefaulted_GetRef();
		Written.EntityId = Point.EntityId;
		Written.Chance = Point.Chance;
		Written.LingerSecondsMin = Point.MinLingerSeconds;
		Written.LingerSecondsMax = Point.MaxLingerSeconds;
		Written.Radius = Point.Radius;
	}

	// Lanes past the ones wanted go, from the end of the order. The default lane is a native
	// subobject and stays; with nothing to hold it is only emptied.
	TArray<UMantleCrowdLaneComponent*> Lanes = GetLanes();
	for (int32 i = Lanes.Num() - 1; i >= In.Lanes.Num(); --i)
	{
		UMantleCrowdLaneComponent* Surplus = Lanes[i];
		if (Surplus == DefaultLane)
		{
			Surplus->ClearSplinePoints();
			continue;
		}
		RemoveInstanceComponent(Surplus);
		Surplus->DestroyComponent();
		Lanes.RemoveAt(i);
	}

	// New lanes are named after the highest numbered one, so they sort after every lane already
	// here. A name a destroyed lane still holds until garbage collection is skipped.
	int32 NextNumber = 0;
	for (const UMantleCrowdLaneComponent* Lane : Lanes)
	{
		if (Lane->GetFName().GetPlainNameString().Equals(TEXT("Lane"), ESearchCase::CaseSensitive))
		{
			NextNumber = FMath::Max(NextNumber, Lane->GetFName().GetNumber());
		}
	}
	int32 Missing = In.Lanes.Num() - Lanes.Num();
	for (; Missing > 0; --Missing)
	{
		FName Name;
		do
		{
			Name = FName(TEXT("Lane"), ++NextNumber);
		}
		while (FindObjectFast<UObject>(this, Name) != nullptr);

		UMantleCrowdLaneComponent* Lane = NewObject<UMantleCrowdLaneComponent>(this, Name, RF_Transactional);
		Lane->SetupAttachment(GetRootComponent());
		// An instance component, so an editor import saves it with the level and lists it in Details.
		AddInstanceComponent(Lane);
		if (GetWorld())
		{
			Lane->RegisterComponent();
		}
	}

	// Filled in ReadProps' order, whatever the names: that order is what the place file keeps.
	Lanes = GetLanes();
	for (int32 i = 0; i < In.Lanes.Num() && i < Lanes.Num(); ++i)
	{
		UMantleCrowdLaneComponent* Lane = Lanes[i];
		const FMantleCrowdLane& Source = In.Lanes[i];
		Lane->Width = Source.Width;
		Lane->bTwoWay = Source.bTwoWay;
		// Straight under the root with no offset of its own, so its local points are the actor's.
		Lane->SetRelativeTransform(FTransform::Identity);

		TArray<FVector> Points = Source.Points;
		const bool bLoop = Points.Num() >= 3 && Points.Last() == Points[0];
		if (bLoop)
		{
			Points.Pop();
		}
		Lane->SetSplinePoints(Points, ESplineCoordinateSpace::Local, /*bUpdateSpline*/ false);
		for (int32 Point = 0; Point < Points.Num(); ++Point)
		{
			Lane->SetSplinePointType(Point, ESplinePointType::Linear, /*bUpdateSpline*/ false);
		}
		Lane->SetClosedLoop(bLoop, /*bUpdateSpline*/ false);
		Lane->UpdateSpline();
	}
}
