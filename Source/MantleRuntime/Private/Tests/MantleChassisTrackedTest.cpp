// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleAssetLibrary.h"
#include "MantleChassisDerive.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// A tracked vehicle's chassis (#448): its axles are its road-wheel pairs (none steered, every one driven and braked), seven a side on the tank and the
// bulldozer; a track runs along each side's road wheels; the tank has a turret on its hull and the bulldozer a blade in front; a resized one keeps its
// tracks in proportion and turns on the spot as fast; and the reader refuses road wheels that do not make two tracks it can turn on.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisTrackedTestPrivate
{
	bool LoadPreset(FAutomationTestBase& Test, const TCHAR* Id, FMantleChassisSpec& OutSpec, FString* OutText = nullptr)
	{
		FString Text;
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) && FMantleChassisSpec::Parse(Text, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("the %s preset reads (%s)"), Id, *Error), bOk);
		if (OutText)
		{
			*OutText = Text;
		}
		return bOk;
	}

	/** Text with Mutate applied to its JSON: a preset changed the way its schema says, not by searching its text. */
	FString Mutated(const FString& Text, TFunctionRef<void(FJsonObject&)> Mutate)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			return FString();
		}
		Mutate(*Root);
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> Child(const FJsonObject& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.TryGetObjectField(Key, Found) && Found ? *Found : nullptr;
	}

	/** The preset's chassis.layout.axles, which Mutate may change. */
	void EditAxles(FJsonObject& Root, TFunctionRef<void(TArray<TSharedPtr<FJsonValue>>&)> Edit)
	{
		const TSharedPtr<FJsonObject> Layout = Child(*Child(Root, TEXT("chassis")), TEXT("layout"));
		TArray<TSharedPtr<FJsonValue>> Axles = Layout->GetArrayField(TEXT("axles"));
		Edit(Axles);
		Layout->SetArrayField(TEXT("axles"), Axles);
	}

	bool Near(const double A, const double B, const double Tolerance = 1.0e-6)
	{
		return FMath::Abs(A - B) <= Tolerance * FMath::Max(1.0, FMath::Abs(B));
	}

	bool NearCm(const FVector& A, const FVector& B)
	{
		return A.Equals(B, 1.0e-3);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrackedSpecTest,
	"Mantle.Vehicle.Chassis.TrackedSpecHasItsTracks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrackedSpecTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrackedTestPrivate;
	FMantleChassisSpec Tank;
	FMantleChassisSpec Dozer;
	if (!LoadPreset(*this, TEXT("tank"), Tank) || !LoadPreset(*this, TEXT("bulldozer"), Dozer))
	{
		return false;
	}
	for (const FMantleChassisSpec* Spec : { &Tank, &Dozer })
	{
		const FString Id = Spec->Id;
		TestTrue(*FString::Printf(TEXT("%s: the tracked type with its block"), *Id), Spec->Type == EMantleChassisType::Tracked && Spec->Tracked.bSet);
		TestTrue(*FString::Printf(TEXT("%s: driven from a car's seat with no steering wheel, and no passenger"), *Id),
			Spec->DriverSeat.Equals(TEXT("car"), ESearchCase::CaseSensitive) && !Spec->bSteeringWheel && !Spec->bPassenger && !Spec->bCenterline);
		// Seven road wheels a side: the layout's axles, a pair each, none steered, each driven and braked.
		TestEqual(*FString::Printf(TEXT("%s: seven axles"), *Id), Spec->Axles.Num(), 7);
		TestEqual(*FString::Printf(TEXT("%s: fourteen road wheels"), *Id), Spec->WheelCount(), 14);
		for (int32 Axle = 0; Axle < Spec->Axles.Num(); ++Axle)
		{
			const FMantleChassisAxle& Pair = Spec->Axles[Axle];
			TestTrue(*FString::Printf(TEXT("%s: axle %d is not steered, and is driven and braked"), *Id, Axle), !Pair.bSteer && Pair.bDrive && Pair.bHandbrake && !Pair.bCenterline);
		}
		// The size keeps the archetype's tracks, so a resized one knows what it was resized from.
		TestTrue(*FString::Printf(TEXT("%s: the archetype's size has its tracks"), *Id),
			Spec->ArchetypeSize.Tracked.bSet && Spec->ArchetypeSize.Tracked.TrackWidth == Spec->Tracked.TrackWidth
			&& Spec->ArchetypeSize.Tracked.PivotTrackSpeed == Spec->Tracked.PivotTrackSpeed);

		// A track runs along each side's road wheels, round the first and last of them, a pad's width about their Y, from the ground over their tops.
		const FMantleChassisBlock Hull = Spec->BodyBlock();
		for (const bool bRight : { false, true })
		{
			const FString Side = FString::Printf(TEXT("%s: the %s track"), *Id, bRight ? TEXT("right") : TEXT("left"));
			const FMantleChassisBlock Track = Spec->TrackBlock(bRight);
			const FVector Front = Spec->WheelCenter(bRight ? 1 : 0);
			const FVector Rear = Spec->WheelCenter(Spec->WheelCount() - (bRight ? 1 : 2));
			TestTrue(*FString::Printf(TEXT("%s runs round the first and last road wheels"), *Side),
				Near(Track.Max.X, Front.X + Spec->WheelRadius) && Near(Track.Min.X, Rear.X - Spec->WheelRadius));
			TestTrue(*FString::Printf(TEXT("%s is a pad's width about the wheels"), *Side),
				Near(Track.Max.Y - Track.Min.Y, Spec->Tracked.TrackWidth) && Near(0.5 * (Track.Min.Y + Track.Max.Y), Front.Y));
			TestTrue(*FString::Printf(TEXT("%s stands on the ground, 10 cm over the wheels' tops"), *Side),
				Track.Min.Z == 0.0 && Near(Track.Max.Z, 2.0 * Spec->WheelRadius + 10.0));
			TestTrue(*FString::Printf(TEXT("%s is on its own side"), *Side), bRight ? Track.Min.Y > 0.0 : Track.Max.Y < 0.0);
		}
		TestTrue(*FString::Printf(TEXT("%s: the road wheels are under the hull"), *Id), Spec->WheelRadius < Spec->Clearance && 0.5 * Spec->Track < Hull.Max.Y);
	}

	// The numbers the contract gives each.
	TestTrue(TEXT("the tank's tracks: 63.5 cm wide, 18 cm pads, 97 cm/s in a pivot, a lateral share of 0.8"),
		Tank.Tracked.TrackWidth == 63.5 && Tank.Tracked.PadPitch == 18.0 && Tank.Tracked.PivotTrackSpeed == 97.0 && Tank.Tracked.LateralShare == 0.8);
	TestTrue(TEXT("the bulldozer's: 76 cm wide, 20 cm pads, 52 cm/s, 0.8"),
		Dozer.Tracked.TrackWidth == 76.0 && Dozer.Tracked.PadPitch == 20.0 && Dozer.Tracked.PivotTrackSpeed == 52.0 && Dozer.Tracked.LateralShare == 0.8);

	// The tank's turret stands on its hull's top, centered at its x; it has no blade.
	TestTrue(TEXT("the tank has a turret and no blade"), Tank.Tracked.bTurret && !Tank.Tracked.bBlade);
	const FMantleChassisBlock Turret = Tank.TurretBlock();
	TestTrue(TEXT("the turret stands on the hull's top"), Turret.Min.Z == Tank.BodyTop() && Near(Turret.Max.Z, Tank.BodyTop() + Tank.Tracked.TurretHeight));
	TestTrue(TEXT("its length, centered at its x"), Near(Turret.Max.X - Turret.Min.X, Tank.Tracked.TurretLength) && Near(0.5 * (Turret.Min.X + Turret.Max.X), Tank.Tracked.TurretX));
	TestTrue(TEXT("its width, centered on the hull"), Near(Turret.Max.Y - Turret.Min.Y, Tank.Tracked.TurretWidth) && Near(Turret.Min.Y, -Turret.Max.Y));
	const FMantleChassisBlock NoBlade = Tank.BladeBlock();
	TestTrue(TEXT("and no blade block"), NoBlade.Min == NoBlade.Max);

	// The bulldozer's blade stands on the ground in front of its hull; it has no turret.
	TestTrue(TEXT("the bulldozer has a blade and no turret"), Dozer.Tracked.bBlade && !Dozer.Tracked.bTurret);
	const FMantleChassisBlock Blade = Dozer.BladeBlock();
	TestTrue(TEXT("the blade stands on the ground, its height tall"), Blade.Min.Z == 0.0 && Near(Blade.Max.Z, Dozer.Tracked.BladeHeight));
	TestTrue(TEXT("in front of the hull"), Blade.Min.X >= Dozer.BodyBlock().Max.X);
	TestTrue(TEXT("its thickness about its x, its width across"),
		Near(Blade.Max.X - Blade.Min.X, Dozer.Tracked.BladeThickness) && Near(0.5 * (Blade.Min.X + Blade.Max.X), Dozer.Tracked.BladeX)
		&& Near(Blade.Max.Y - Blade.Min.Y, Dozer.Tracked.BladeWidth));
	const FMantleChassisBlock NoTurret = Dozer.TurretBlock();
	TestTrue(TEXT("and no turret block"), NoTurret.Min == NoTurret.Max);

	// Nothing else has tracks.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Other;
		FString Text;
		FString Error;
		if (Id == TEXT("tank") || Id == TEXT("bulldozer") || !FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !FMantleChassisSpec::Parse(Text, Other, Error))
		{
			continue;
		}
		const FMantleChassisBlock Track = Other.TrackBlock(false);
		TestTrue(*FString::Printf(TEXT("%s has no tracked block and no track"), *Id), !Other.Tracked.bSet && Other.Type != EMantleChassisType::Tracked && Track.Min == Track.Max);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrackedBodyTest,
	"Mantle.Vehicle.Chassis.TrackedBodyIsWheelsAndBlocksOnTheRoot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrackedBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrackedTestPrivate;
	FMantleChassisSpec Tank;
	if (!LoadPreset(*this, TEXT("tank"), Tank))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("it makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Tank, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& Desc = Asset.Desc;
	TestTrue(TEXT("a tracked vehicle"), Desc.MovementType == EMantleVehicleMovementKind::Tracked);
	TestTrue(TEXT("its tracks are the preset's"), Desc.Tracked.TrackWidth == 63.5 && Desc.Tracked.PadPitch == 18.0 && Desc.Tracked.PivotTrackSpeed == 97.0
		&& Desc.Tracked.LateralShare == 0.8 && Desc.Tracked.Rollers.Num() == 0);
	TestEqual(TEXT("its road wheels roll as a track does"), Desc.TiresRollingResistance, 0.04);
	TestEqual(TEXT("a root and fourteen road-wheel bones"), Asset.Body.Bones.Num(), 15);
	if (TestEqual(TEXT("fourteen wheels in the document"), Desc.Wheels.Num(), 14))
	{
		TestEqual(TEXT("numbered from the front"), Desc.Wheels[0].Bone, FName(TEXT("wheel_a1_l")));
		TestEqual(TEXT("to the seventh axle's right"), Desc.Wheels[13].Bone, FName(TEXT("wheel_a7_r")));
	}
	FString Why;
	TestTrue(*FString::Printf(TEXT("its road wheels make two tracks it can turn on (%s)"), *Why), FMantleVehicleAssetReader::CheckTrackedLayout(Desc, Asset.WheelPositions, Why));
	// Only the hull collides.
	if (TestEqual(TEXT("one collision box"), Desc.Collision.Num(), 1))
	{
		const FMantleChassisBlock Hull = Tank.BodyBlock();
		TestTrue(TEXT("the hull's"), NearCm(Desc.Collision[0].Center, 0.5 * (Hull.Min + Hull.Max)) && NearCm(Desc.Collision[0].Extent, 0.5 * (Hull.Max - Hull.Min)));
	}

	// The gray body: the hull, the road wheels on their bones and the turret, then the two tracks in a section of their own on the root.
	if (!TestEqual(TEXT("one LOD"), Asset.Body.LODs.Num(), 1))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
	// A box is 24 vertices and 12 triangles, a wheel 74 and 48: the hull, the turret and two tracks, and fourteen wheels.
	TestEqual(TEXT("the vertices"), LOD.Positions.Num(), 4 * 24 + 14 * 74);
	TestEqual(TEXT("the triangles"), LOD.NumTriangles(), 4 * 12 + 14 * 48);
	if (TestEqual(TEXT("two sections"), LOD.Sections.Num(), 2))
	{
		const FMantleSkinnedMeshSection& Body = LOD.Sections[0];
		const FMantleSkinnedMeshSection& Tracks = LOD.Sections[1];
		TestTrue(TEXT("the body's first"), Body.FirstIndex == 0 && Body.MaterialIndex == 0 && Body.MaterialName == FName(TEXT("chassis")));
		TestTrue(TEXT("then the tracks', in a slot of their own, to the last index"),
			Tracks.FirstIndex == Body.IndexCount && Tracks.FirstIndex + Tracks.IndexCount == LOD.Indices.Num() && Tracks.MaterialIndex == 1);
		TestEqual(TEXT("two boxes of track"), Tracks.IndexCount, 2 * 12 * 3);
		TestEqual(TEXT("named Track"), Tracks.MaterialName, FMantleVehicleChassisBuilder::TrackSectionName());
		TestEqual(TEXT("which is Track"), FMantleVehicleChassisBuilder::TrackSectionName(), FName(TEXT("Track")));
		// The tracks' vertices are on the root: the gray belt does not flex with the road wheels.
		bool bTracksOnRoot = true;
		for (int32 Index = Tracks.FirstIndex; Index < Tracks.FirstIndex + Tracks.IndexCount; ++Index)
		{
			bTracksOnRoot = bTracksOnRoot && LOD.InfluenceBones[static_cast<int32>(LOD.Indices[Index]) * LOD.NumInfluences] == 0;
		}
		TestTrue(TEXT("every track vertex is on the root"), bTracksOnRoot);
	}
	// The track section is drawn with Mantle's built-in track material: a library material, so nothing is downloaded for it.
	const FString* TrackMaterial = Desc.Materials.Find(FMantleVehicleChassisBuilder::TrackSectionName().ToString());
	TestTrue(TEXT("the Track section is drawn with ast_mat_track"), TrackMaterial && *TrackMaterial == TEXT("ast_mat_track"));
	const FMantleAssetEntry* Entry = FMantleAssetLibrary::Get().Find(FMantleVehicleChassisBuilder::TrackMaterialId());
	TestTrue(TEXT("which is a built-in material"), Entry && Entry->Kind == EMantleAssetKind::Material);
	TestFalse(TEXT("so the vehicle downloads no material for it"), Desc.MaterialIds().Contains(FString(FMantleVehicleChassisBuilder::TrackMaterialId())));

	// Every road wheel is drawn on its own bone, with nothing else of the body on one.
	int32 OnWheels = 0;
	for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
	{
		OnWheels += LOD.InfluenceBones[Vertex * LOD.NumInfluences] != 0 ? 1 : 0;
	}
	TestEqual(TEXT("each road wheel's vertices, and only those, are on a wheel bone"), OnWheels, 14 * 74);

	// The bulldozer's: its blade in place of a turret, the same tracks.
	FMantleChassisSpec Dozer;
	FMantleVehicleAsset DozerAsset;
	if (LoadPreset(*this, TEXT("bulldozer"), Dozer)
		&& TestTrue(*FString::Printf(TEXT("the bulldozer makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Dozer, DozerAsset, Error)))
	{
		TestEqual(TEXT("the bulldozer's vertices: the hull, the blade, two tracks and fourteen wheels"), DozerAsset.Body.LODs[0].Positions.Num(), 4 * 24 + 14 * 74);
		TestEqual(TEXT("and its two sections"), DozerAsset.Body.LODs[0].Sections.Num(), 2);
		TestTrue(TEXT("its tracks are the preset's"), DozerAsset.Desc.Tracked.TrackWidth == 76.0 && DozerAsset.Desc.Tracked.PivotTrackSpeed == 52.0);
	}

	// No other chassis has a second section or a track material.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Other;
		FMantleVehicleAsset OtherAsset;
		FString Text;
		if (Id == TEXT("tank") || Id == TEXT("bulldozer") || !FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error)
			|| !FMantleVehicleChassisBuilder::MakeAsset(Text, OtherAsset, Error))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: one section and no materials"), *Id), OtherAsset.Body.LODs[0].Sections.Num() == 1 && OtherAsset.Desc.Materials.Num() == 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrackedPartsTest,
	"Mantle.Vehicle.Chassis.TrackedPresetNeedsItsParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrackedPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrackedTestPrivate;
	FMantleChassisSpec Tank;
	FString Text;
	FString Sedan;
	FString Error;
	if (!LoadPreset(*this, TEXT("tank"), Tank, &Text) || !TestTrue(TEXT("the sedan's text reads"), FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sedan"), Sedan, Error)))
	{
		return false;
	}
	// A change to the tank that is no longer a preset, and the refusal says Expect.
	auto Refuses = [this](const TCHAR* Name, const FString& Changed, const TCHAR* Expect)
	{
		FMantleChassisSpec Spec;
		FString Why;
		if (TestFalse(*FString::Printf(TEXT("%s is refused"), Name), FMantleChassisSpec::Parse(Changed, Spec, Why)))
		{
			TestTrue(*FString::Printf(TEXT("%s: the refusal says '%s' (it says: %s)"), Name, Expect, *Why), Why.Contains(Expect));
		}
	};
	{
		FMantleChassisSpec Again;
		TestTrue(*FString::Printf(TEXT("the tank written back reads (%s)"), *Error), FMantleChassisSpec::Parse(Mutated(Text, [](FJsonObject&) {}), Again, Error));
	}
	Refuses(TEXT("a tracked preset with no tracked block"), Mutated(Text, [](FJsonObject& Root) { Child(Root, TEXT("chassis"))->RemoveField(TEXT("tracked")); }), TEXT("tracked"));
	// (On a car's two axles: seven are past a car's four, which it would be refused for first.)
	Refuses(TEXT("a car with a tracked block"), Mutated(Text, [](FJsonObject& Root)
	{
		Root.SetStringField(TEXT("type"), TEXT("car"));
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles) { Axles.SetNum(2); });
	}), TEXT("type tracked and its chassis.tracked"));
	Refuses(TEXT("a track with no width"), Mutated(Text, [](FJsonObject& Root) { Child(*Child(Root, TEXT("chassis")), TEXT("tracked"))->RemoveField(TEXT("trackWidth")); }),
		TEXT("chassis.tracked.trackWidth"));
	Refuses(TEXT("a pivot speed of 0"), Mutated(Text, [](FJsonObject& Root) { Child(*Child(Root, TEXT("chassis")), TEXT("tracked"))->SetNumberField(TEXT("pivotTrackSpeed"), 0); }),
		TEXT("chassis.tracked.pivotTrackSpeed"));
	Refuses(TEXT("a turret with no height"), Mutated(Text, [](FJsonObject& Root)
	{
		Child(*Child(*Child(Root, TEXT("chassis")), TEXT("tracked")), TEXT("turret"))->RemoveField(TEXT("height"));
	}), TEXT("chassis.tracked.turret.height"));
	Refuses(TEXT("a turret that is not an object"), Mutated(Text, [](FJsonObject& Root)
	{
		Child(*Child(Root, TEXT("chassis")), TEXT("tracked"))->SetNumberField(TEXT("turret"), 1);
	}), TEXT("chassis.tracked.turret"));
	Refuses(TEXT("a steered road-wheel pair"), Mutated(Text, [](FJsonObject& Root)
	{
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles) { Axles[2]->AsObject()->SetBoolField(TEXT("steer"), true); });
	}), TEXT("axles[2]"));
	Refuses(TEXT("a road-wheel pair that is not driven"), Mutated(Text, [](FJsonObject& Root)
	{
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles) { Axles[0]->AsObject()->SetBoolField(TEXT("drive"), false); });
	}), TEXT("drive true"));
	Refuses(TEXT("a road-wheel pair with no handbrake"), Mutated(Text, [](FJsonObject& Root)
	{
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles) { Axles[6]->AsObject()->RemoveField(TEXT("handbrake")); });
	}), TEXT("handbrake true"));
	Refuses(TEXT("two road wheels a side"), Mutated(Text, [](FJsonObject& Root)
	{
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles) { Axles.SetNum(2); });
	}), TEXT("3 to 10 axles"));
	Refuses(TEXT("eleven road wheels a side"), Mutated(Text, [](FJsonObject& Root)
	{
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles)
		{
			// A copy first: an element of the array is not added to it.
			const TSharedPtr<FJsonValue> First = Axles[0];
			while (Axles.Num() < 11)
			{
				Axles.Add(First);
			}
		});
	}), TEXT("3 to 10 axles"));
	Refuses(TEXT("a tank on a pilot's seat"), Mutated(Text, [](FJsonObject& Root) { Child(Root, TEXT("seats"))->SetStringField(TEXT("driver"), TEXT("pilot")); }),
		TEXT("car's seat"));
	// The tracked limit is a tracked vehicle's alone: a car still has two to four axles.
	Refuses(TEXT("a car on five axles"), Mutated(Sedan, [](FJsonObject& Root)
	{
		EditAxles(Root, [](TArray<TSharedPtr<FJsonValue>>& Axles)
		{
			const TSharedPtr<FJsonValue> Last = Axles.Last();
			while (Axles.Num() < 5)
			{
				Axles.Add(Last);
			}
		});
	}), TEXT("2 to 4 axles"));
	EMantleChassisType Type = EMantleChassisType::Car;
	TestTrue(TEXT("the type name reads back"), FMantleChassisSpec::ParseType(TEXT("tracked"), Type) && Type == EMantleChassisType::Tracked
		&& FString(FMantleChassisSpec::TypeName(EMantleChassisType::Tracked)).Equals(TEXT("tracked"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrackedScaledTest,
	"Mantle.Vehicle.Chassis.TrackedScaledKeepsItsTracksInProportion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrackedScaledTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrackedTestPrivate;
	FMantleChassisSpec Tank;
	if (!LoadPreset(*this, TEXT("tank"), Tank))
	{
		return false;
	}
	const FMantleChassisTracked& Own = Tank.Tracked;
	// How fast it turns on the spot, rad/s: each track at the pivot speed, opposite ways, about the middle of the gauge.
	const double OwnPivotRate = 2.0 * Own.PivotTrackSpeed / Tank.Track;
	const FMantleChassisBlock OwnTrack = Tank.TrackBlock(true);
	const FMantleChassisBlock OwnTurret = Tank.TurretBlock();
	for (const double Factor : { 0.5, 0.7, 1.3, 1.6 })
	{
		const FString Label = FString::Printf(TEXT("x%g"), Factor);
		const FMantleChassisSpec Sized = FMantleChassisSpec::Scaled(Tank, Factor);
		const FMantleChassisTracked& Grown = Sized.Tracked;
		TestTrue(*FString::Printf(TEXT("%s: the track's width and pad length scale"), *Label), Near(Grown.TrackWidth, Factor * Own.TrackWidth) && Near(Grown.PadPitch, Factor * Own.PadPitch));
		TestTrue(*FString::Printf(TEXT("%s: the turret scales"), *Label), Near(Grown.TurretLength, Factor * Own.TurretLength) && Near(Grown.TurretWidth, Factor * Own.TurretWidth)
			&& Near(Grown.TurretHeight, Factor * Own.TurretHeight) && Near(Grown.TurretX, Factor * Own.TurretX));
		TestTrue(*FString::Printf(TEXT("%s: the pivot speed scales"), *Label), Near(Grown.PivotTrackSpeed, Factor * Own.PivotTrackSpeed));
		TestEqual(*FString::Printf(TEXT("%s: the lateral share is a ratio, the same"), *Label), Grown.LateralShare, Own.LateralShare);
		TestTrue(*FString::Printf(TEXT("%s: so it turns on the spot as fast as the preset"), *Label), Near(2.0 * Grown.PivotTrackSpeed / Sized.Track, OwnPivotRate));
		TestTrue(*FString::Printf(TEXT("%s: the archetype's tracks stay the preset's"), *Label), Sized.ArchetypeSize.Tracked.PivotTrackSpeed == Own.PivotTrackSpeed);
		// The drawn blocks are the preset's, scaled: every corner Factor times as far from the origin.
		const FMantleChassisBlock Track = Sized.TrackBlock(true);
		const FMantleChassisBlock Turret = Sized.TurretBlock();
		TestTrue(*FString::Printf(TEXT("%s: the track block scales"), *Label), NearCm(Track.Min, Factor * OwnTrack.Min) && NearCm(Track.Max, Factor * OwnTrack.Max));
		TestTrue(*FString::Printf(TEXT("%s: the turret block scales"), *Label), NearCm(Turret.Min, Factor * OwnTurret.Min) && NearCm(Turret.Max, Factor * OwnTurret.Max));

		// Its document carries the scaled tracks, and it still turns on the spot: the contact length and the gauge grow together.
		FMantleVehicleAsset Asset;
		FString Error;
		if (TestTrue(*FString::Printf(TEXT("%s: it makes an asset (%s)"), *Label, *Error), FMantleVehicleChassisBuilder::MakeAsset(Sized, Asset, Error)))
		{
			// (Through the document's text: equal to its printed digits.)
			TestTrue(*FString::Printf(TEXT("%s: the document's tracks are the scaled ones"), *Label), Near(Asset.Desc.Tracked.PivotTrackSpeed, Grown.PivotTrackSpeed, 1.0e-9)
				&& Near(Asset.Desc.Tracked.TrackWidth, Grown.TrackWidth, 1.0e-9) && Near(Asset.Desc.Tracked.PadPitch, Grown.PadPitch, 1.0e-9)
				&& Near(Asset.Desc.Tracked.LateralShare, Own.LateralShare, 1.0e-9));
		}
	}
	// A preset at its own size is untouched.
	const FMantleChassisSpec Same = FMantleChassisSpec::Scaled(Tank, 1.0);
	TestTrue(TEXT("scaled by 1 it is the preset"), Same.Tracked.PivotTrackSpeed == Own.PivotTrackSpeed && Same.Tracked.TrackWidth == Own.TrackWidth && !Same.Derived.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisTrackedLayoutTest,
	"Mantle.Vehicle.Chassis.TrackedLayoutIsTwoTracksItCanTurnOn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisTrackedLayoutTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisTrackedTestPrivate;
	FMantleChassisSpec Tank;
	FMantleVehicleAsset Asset;
	FString Error;
	if (!LoadPreset(*this, TEXT("tank"), Tank) || !TestTrue(*FString::Printf(TEXT("the tank makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Tank, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& Desc = Asset.Desc;
	const TArray<FVector>& Positions = Asset.WheelPositions;
	auto Check = [](const FMantleVehicleDesc& D, const TArray<FVector>& At, FString& OutWhy) { return FMantleVehicleAssetReader::CheckTrackedLayout(D, At, OutWhy); };
	FString Why;
	TestTrue(*FString::Printf(TEXT("the tank's own layout turns (%s)"), *Why), Check(Desc, Positions, Why));

	// The contract's numbers: a 525 cm contact (490 cm first to last road wheel, plus a radius) over a 310 cm gauge, times 0.8, is 1.35.
	{
		double MinX = TNumericLimits<double>::Max();
		double MaxX = -TNumericLimits<double>::Max();
		for (const FVector& Position : Positions)
		{
			MinX = FMath::Min(MinX, Position.X);
			MaxX = FMath::Max(MaxX, Position.X);
		}
		TestTrue(TEXT("490 cm from the first road wheel to the last"), Near(MaxX - MinX, 490.0));
		TestTrue(TEXT("on a 310 cm gauge"), Near(Positions[1].Y - Positions[0].Y, 310.0));
	}

	// A wheel on the centerline is on neither track.
	TArray<FVector> Moved = Positions;
	Moved[4].Y = 0.0;
	TestFalse(TEXT("a road wheel on the centerline is refused"), Check(Desc, Moved, Why));
	TestTrue(*FString::Printf(TEXT("and named (%s)"), *Why), Why.Contains(TEXT("wheel 4")) && Why.Contains(TEXT("centerline")));
	// Within a centimeter of it, its side cannot be told either; a centimeter out, it can.
	Moved[4].Y = -0.5;
	TestFalse(TEXT("a road wheel half a centimeter off the centerline is refused"), Check(Desc, Moved, Why));
	Moved[4].Y = 0.99;
	TestFalse(TEXT("and one 0.99 cm off"), Check(Desc, Moved, Why));

	// Each track has 3 to 10 road wheels: five of the left's moved across leave it two (and the right twelve).
	Moved = Positions;
	for (int32 Index = 0, Done = 0; Index < Moved.Num() && Done < 5; ++Index)
	{
		if (Moved[Index].Y < 0.0)
		{
			Moved[Index].Y = -Moved[Index].Y;
			++Done;
		}
	}
	TestFalse(TEXT("two road wheels on a track is refused"), Check(Desc, Moved, Why));
	TestTrue(*FString::Printf(TEXT("and says which track (%s)"), *Why), Why.Contains(TEXT("2 road wheels on its left")));
	Moved = Positions;
	for (int32 Index = 0, Done = 0; Index < Moved.Num() && Done < 4; ++Index)
	{
		if (Moved[Index].Y < 0.0)
		{
			Moved[Index].Y = -Moved[Index].Y;
			++Done;
		}
	}
	TestFalse(TEXT("eleven road wheels on a track is refused"), Check(Desc, Moved, Why));
	TestTrue(*FString::Printf(TEXT("and says which track (%s)"), *Why), Why.Contains(TEXT("11 road wheels on its right")));

	// Turning on the spot: l / B x lateralShare under 2. The tank's 525 / 310 is 1.694, so a lateral share of 1.18 still turns and 1.19 does not.
	FMantleVehicleDesc Sliding = Desc;
	Sliding.Tracked.LateralShare = 1.18;
	TestTrue(*FString::Printf(TEXT("a lateral share of 1.18 still pivots (%s)"), *Why), Check(Sliding, Positions, Why));
	Sliding.Tracked.LateralShare = 1.19;
	TestFalse(TEXT("1.19 cannot"), Check(Sliding, Positions, Why));
	TestTrue(*FString::Printf(TEXT("and says so (%s)"), *Why), Why.Contains(TEXT("can't turn on the spot")));
	// The same wheels on half the gauge are too long for it.
	Moved = Positions;
	for (FVector& Position : Moved)
	{
		Position.Y *= 0.5;
	}
	TestFalse(TEXT("tracks too long for their gauge are refused"), Check(Desc, Moved, Why));

	// A wheel position for every wheel; and the rule is a tracked vehicle's alone.
	TArray<FVector> Short = Positions;
	Short.Pop();
	TestFalse(TEXT("a position missing is refused"), Check(Desc, Short, Why));
	FMantleVehicleDesc Car = Desc;
	Car.MovementType = EMantleVehicleMovementKind::Wheeled;
	TestTrue(TEXT("a wheeled vehicle's wheels may stand anywhere"), Check(Car, Moved, Why));

	// A sprocket, idler or return roller names a bone of the body, which spins: not the root, and not a road wheel's (as an aerofoil's or a spin bone's).
	FMantleVehicleDesc WithRoller = Desc;
	FMantleVehicleTrackRollerDesc& Roller = WithRoller.Tracked.Rollers.AddDefaulted_GetRef();
	Roller.Radius = 30.0;
	Roller.Bone = FName(TEXT("sprocket_l"));
	TestFalse(TEXT("a roller on a bone the body lacks is refused"), FMantleVehicleAssetReader::ResolveMovementBones(WithRoller, Asset.Body.Bones, Why));
	TestTrue(*FString::Printf(TEXT("and named (%s)"), *Why), Why.Contains(TEXT("track roller 0")) && Why.Contains(TEXT("not in its body")));
	WithRoller.Tracked.Rollers[0].Bone = FMantleVehicleChassisBuilder::RootBoneName();
	TestFalse(TEXT("a roller on the root is refused"), FMantleVehicleAssetReader::ResolveMovementBones(WithRoller, Asset.Body.Bones, Why));
	WithRoller.Tracked.Rollers[0].Bone = FName(TEXT("WHEEL_A1_L"));
	TestFalse(TEXT("a roller on a road wheel's bone is refused"), FMantleVehicleAssetReader::ResolveMovementBones(WithRoller, Asset.Body.Bones, Why));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
