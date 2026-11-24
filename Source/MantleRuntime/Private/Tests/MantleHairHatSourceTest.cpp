// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairHatSource.h"

#include "MantleAccessoryPlacement.h"
#include "MantleHairHatFit.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Hair under hats (#389), the step that runs in the cache's hair fit task: where a hat is in the hair's space, the fit key's hat
// part, and the whole step on a hair mesh. The hat is a dome of radius 10 cm on a head of radius 9 cm, the head bone 160 cm up.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleHairHatSourceTestPrivate
{
	constexpr float HeadZ = 160.0f;

	/** root, then head; the head's own pose puts it HeadZ up. */
	FMantleSkinnedMesh MakeBody()
	{
		FMantleSkinnedMesh Body;
		Body.Bones.Add({ FName(TEXT("root")), INDEX_NONE });
		Body.Bones.Add({ FName(TEXT("head")), 0 });
		Body.ReferencePose.Add(FTransform::Identity);
		Body.ReferencePose.Add(FTransform(FQuat::Identity, FVector(0.0, 0.0, HeadZ)));
		return Body;
	}

	/** A sphere cap of radius R about the origin from the +Z pole down to ThetaMaxDeg, then moved Offset. Triangles two-sided; open at the bottom. */
	void Dome(const float R, const float ThetaMaxDeg, const FVector3f& Offset, TArray<FVector3f>& OutP, TArray<uint32>& OutI)
	{
		constexpr int32 Segments = 48;
		const int32 Steps = FMath::Max(4, FMath::CeilToInt(ThetaMaxDeg / 3.0f));
		const uint32 Base = OutP.Num();
		for (int32 Ring = 0; Ring <= Steps; ++Ring)
		{
			const float T = FMath::DegreesToRadians(ThetaMaxDeg * static_cast<float>(Ring) / static_cast<float>(Steps));
			for (int32 S = 0; S < Segments; ++S)
			{
				const float A = 2.0f * UE_PI * static_cast<float>(S) / static_cast<float>(Segments);
				OutP.Add(Offset + FVector3f(R * FMath::Sin(T) * FMath::Cos(A), R * FMath::Sin(T) * FMath::Sin(A), R * FMath::Cos(T)));
			}
		}
		for (int32 Ring = 0; Ring < Steps; ++Ring)
		{
			for (int32 S = 0; S < Segments; ++S)
			{
				const uint32 A = Base + Ring * Segments + S;
				const uint32 B = Base + Ring * Segments + (S + 1) % Segments;
				const uint32 C = Base + (Ring + 1) * Segments + S;
				const uint32 D = Base + (Ring + 1) * Segments + (S + 1) % Segments;
				OutI.Append({ A, C, B, B, C, D });
			}
		}
	}

	/** The wearer's face: just the head skin (a whole sphere of radius 9 at the head) in a section named "skin". */
	FMantleSkinnedMesh MakeFace()
	{
		FMantleSkinnedMesh Face;
		FMantleSkinnedMeshLOD& LOD = Face.LODs.AddDefaulted_GetRef();
		Dome(9.0f, 180.0f, FVector3f(0.0f, 0.0f, HeadZ), LOD.Positions, LOD.Indices);
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialName = FName(TEXT("MI_Head_skin"));
		Section.IndexCount = LOD.Indices.Num();
		return Face;
	}

	/** The head centre the step takes from MakeFace's skin: 7 cm below its top. */
	FVector3f HeadCentre()
	{
		return FVector3f(0.0f, 0.0f, HeadZ + 9.0f - FMantleHairHatFit::HeadCentreBelowTopCm);
	}

	/** The hat as MRM1 bytes: a dome of radius 10 about its own origin (the hat pivot). */
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> MakeHatBytes()
	{
		TArray<FVector3f> P;
		TArray<uint32> I;
		Dome(10.0f, 110.0f, FVector3f::ZeroVector, P, I);
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
		const auto PutU32 = [&Bytes](const uint32 Value) { Bytes->Append(reinterpret_cast<const uint8*>(&Value), 4); };
		const auto PutF32 = [&Bytes](const float Value) { Bytes->Append(reinterpret_cast<const uint8*>(&Value), 4); };
		Bytes->Append(reinterpret_cast<const uint8*>("MRM1"), 4);
		PutU32(1);
		PutU32(0);
		PutU32(static_cast<uint32>(P.Num()));
		PutU32(static_cast<uint32>(I.Num()));
		const FBox3f Box(P.GetData(), P.Num());
		PutF32(Box.Min.X); PutF32(Box.Min.Y); PutF32(Box.Min.Z);
		PutF32(Box.Max.X); PutF32(Box.Max.Y); PutF32(Box.Max.Z);
		for (const FVector3f& Point : P)
		{
			PutF32(Point.X); PutF32(Point.Y); PutF32(Point.Z);
		}
		for (const uint32 Index : I)
		{
			PutU32(Index);
		}
		return Bytes;
	}

	FMantleHairHatSource MakeSource()
	{
		FMantleHairHatSource Source;
		Source.SlotId = TEXT("acc_hat");
		Source.MeshHash = TEXT("hat-a");
		Source.MeshBytes = MakeHatBytes();
		Source.BoneName = FName(TEXT("head"));
		return Source;
	}

	/** Where the dome is in the hair's space for MakeSource (its pivot on the head bone). */
	void PlacedDome(TArray<FVector3f>& OutP, TArray<uint32>& OutI)
	{
		Dome(10.0f, 110.0f, FVector3f(0.0f, 0.0f, HeadZ), OutP, OutI);
	}

	/** One loose triangle per entry of Corners (three points each), as a hair LOD bound to the head bone. */
	FMantleSkinnedMeshLOD MakeHairLOD(const TArray<FVector3f>& Corners)
	{
		FMantleSkinnedMeshLOD LOD;
		LOD.Positions = Corners;
		LOD.Normals.Init(FVector3f(0.0f, 0.0f, 1.0f), Corners.Num());
		LOD.UVs.Init(FVector2f(0.0f, 0.0f), Corners.Num());
		for (int32 V = 0; V < Corners.Num(); ++V)
		{
			LOD.Indices.Add(V);
		}
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.IndexCount = LOD.Indices.Num();
		LOD.NumInfluences = 1;
		LOD.InfluenceBones.Init(1, Corners.Num());
		LOD.InfluenceWeights.Init(255, Corners.Num());
		return LOD;
	}

	/** A hair mesh with the same loose triangles on as many LODs as asked. */
	FMantleSkinnedMesh MakeHair(const TArray<FVector3f>& Corners, const int32 NumLODs = 1)
	{
		FMantleSkinnedMesh Hair = MakeBody();
		for (int32 LOD = 0; LOD < NumLODs; ++LOD)
		{
			Hair.LODs.Add(MakeHairLOD(Corners));
		}
		return Hair;
	}

	/** The point on the ray from the head centre in a direction (polar angle from +Z, azimuth), Distance from the centre. */
	FVector3f OnRay(const float ThetaDeg, const float PhiDeg, const float Distance)
	{
		const float T = FMath::DegreesToRadians(ThetaDeg);
		const float F = FMath::DegreesToRadians(PhiDeg);
		return HeadCentre() + Distance * FVector3f(FMath::Sin(T) * FMath::Cos(F), FMath::Sin(T) * FMath::Sin(F), FMath::Cos(T));
	}

	/** How far from the head centre the placed dome is along a ray. */
	float HatAlong(const float ThetaDeg, const float PhiDeg)
	{
		TArray<FVector3f> P;
		TArray<uint32> I;
		PlacedDome(P, I);
		return FMantleHairHatGrid::FirstHitBruteForce(HeadCentre(), (OnRay(ThetaDeg, PhiDeg, 1.0f) - HeadCentre()).GetSafeNormal(), P, I);
	}

	/**
	 * Four loose triangles: 0 tucked (1.5 cm inside the dome), 1 with one corner 0.2 cm beyond it (pulled in, kept), 2 wholly 1 cm beyond
	 * (cut), 3 below the dome (no hat on its rays, untouched). The two bases of the rays are at least 30 degrees from the pole.
	 */
	TArray<FVector3f> FourTriangles()
	{
		TArray<FVector3f> C;
		C.Append({ OnRay(30.0f, 0.0f, HatAlong(30.0f, 0.0f) - 1.5f), OnRay(30.0f, 20.0f, HatAlong(30.0f, 20.0f) - 1.5f), OnRay(45.0f, 0.0f, HatAlong(45.0f, 0.0f) - 1.5f) });
		C.Append({ OnRay(30.0f, 100.0f, HatAlong(30.0f, 100.0f) + 0.2f), OnRay(30.0f, 120.0f, HatAlong(30.0f, 120.0f) - 1.5f), OnRay(45.0f, 100.0f, HatAlong(45.0f, 100.0f) - 1.5f) });
		C.Append({ OnRay(40.0f, 200.0f, HatAlong(40.0f, 200.0f) + 1.0f), OnRay(40.0f, 220.0f, HatAlong(40.0f, 220.0f) + 1.0f), OnRay(60.0f, 200.0f, HatAlong(60.0f, 200.0f) + 1.0f) });
		C.Append({ OnRay(150.0f, 0.0f, 11.0f), OnRay(150.0f, 20.0f, 11.0f), OnRay(160.0f, 0.0f, 11.0f) });
		return C;
	}

	/** The placed hat dome as a hood: the same triangles a hat there would be tested against, now as fitted outerwear. */
	TSharedPtr<const FMantleHairHood, ESPMode::ThreadSafe> MakeDomeHood()
	{
		TSharedRef<FMantleHairHood, ESPMode::ThreadSafe> Hood = MakeShared<FMantleHairHood, ESPMode::ThreadSafe>();
		PlacedDome(Hood->Positions, Hood->Indices);
		return Hood;
	}

	FMantleHairHatSource MakeHoodSource()
	{
		FMantleHairHatSource Source;
		Source.SlotId = TEXT("outerwear");
		Source.MeshHash = TEXT("jacket-a");
		Source.Hood = MakeDomeHood();
		return Source;
	}

	/** Three slots per vertex: the head bone (1) gets HeadWeight, the root (0) the rest, and a spare slot on the root with none. */
	void AddJacketVertex(FMantleSkinnedMeshLOD& LOD, const int32 Number, const uint8 HeadWeight)
	{
		LOD.Positions.Add(FVector3f(static_cast<float>(Number), 2.0f * Number, 3.0f * Number));
		LOD.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
		LOD.UVs.Add(FVector2f(0.0f, 0.0f));
		LOD.InfluenceBones.Add(1);
		LOD.InfluenceWeights.Add(HeadWeight);
		LOD.InfluenceBones.Add(0);
		LOD.InfluenceWeights.Add(static_cast<uint8>(255 - HeadWeight));
		LOD.InfluenceBones.Add(0);
		LOD.InfluenceWeights.Add(0);
	}

	/**
	 * Fitted outerwear on the body's bones, five triangles of loose vertices and a sixth sharing two corners with the first:
	 * 0 all head (kept), 1 one corner 127/255 on the head (dropped), 2 one corner 128/255 (kept), 3 head weight split over two slots
	 * of the same bone, 100 + 28 (kept), 4 all on the root (dropped), 5 shares vertices 0 and 2 with triangle 0 and adds 6 (kept).
	 */
	FMantleSkinnedMesh MakeJacket()
	{
		FMantleSkinnedMesh Jacket = MakeBody();
		FMantleSkinnedMeshLOD& LOD = Jacket.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = 3;
		const uint8 Weights[] = { 255, 255, 255,  255, 255, 127,  255, 255, 128,  0, 0, 0,  0, 0, 0,  255 };
		for (int32 V = 0; V < 15; ++V)
		{
			AddJacketVertex(LOD, V, Weights[V]);
		}
		AddJacketVertex(LOD, 15, 255);
		// Triangle 3 (vertices 9, 10, 11): the head weight lives in two slots of the same bone, 100 and 28 (128 of 255), the root has the other 127.
		for (const int32 V : { 9, 10, 11 })
		{
			LOD.InfluenceBones[V * 3] = 1;
			LOD.InfluenceWeights[V * 3] = 100;
			LOD.InfluenceBones[V * 3 + 1] = 1;
			LOD.InfluenceWeights[V * 3 + 1] = 28;
			LOD.InfluenceBones[V * 3 + 2] = 0;
			LOD.InfluenceWeights[V * 3 + 2] = 127;
		}
		// Triangle 4 (vertices 12, 13, 14) on the root alone: head weight 0, set above; its slot 0 names the head with weight 0.
		LOD.Indices = { 0, 1, 2,  3, 4, 5,  6, 7, 8,  9, 10, 11,  12, 13, 14,  0, 2, 15 };
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.IndexCount = LOD.Indices.Num();
		return Jacket;
	}

	FString ValidityOf(const FMantleSkinnedMesh& Mesh)
	{
		FString Error;
		return Mesh.Validate(Error) ? FString() : Error;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatSourceKeyTest,
	"Mantle.Runtime.HairHatSource.Key",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatSourceKeyTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatSourceTestPrivate;
	TestTrue(TEXT("no hat, no key piece"), FMantleHairHatSource::KeyFor(TConstArrayView<FMantleHairHatSource>()).IsEmpty());

	const FMantleHairHatSource Base = MakeSource();
	const FString BaseKey = FMantleHairHatSource::KeyFor(MakeArrayView(&Base, 1));
	TestTrue(TEXT("the key names the rule version"), BaseKey.Contains(FString::Printf(TEXT("hathair-v%d"), FMantleHairHatSource::RuleVersion)));
	TestTrue(TEXT("the key names the slot and the mesh"), BaseKey.Contains(TEXT("acc_hat=hat-a")));
	TestEqual(TEXT("the same hat is the same key"), FMantleHairHatSource::KeyFor(MakeArrayView(&Base, 1)), BaseKey);
	// The dev comparison's rule b is a fit of its own; rule a keeps the key fits have always had.
	TestEqual(TEXT("rule a is the default key"), FMantleHairHatSource::KeyFor(MakeArrayView(&Base, 1), EMantleHairHatAim::PokingThrough), BaseKey);
	const FString CardsKey = FMantleHairHatSource::KeyFor(MakeArrayView(&Base, 1), EMantleHairHatAim::UnderHatCards);
	TestNotEqual(TEXT("rule b is another key"), CardsKey, BaseKey);
	TestTrue(TEXT("rule b's key says so"), CardsKey.Contains(FString::Printf(TEXT("hathair-v%d-cards["), FMantleHairHatSource::RuleVersion)));
	TestTrue(TEXT("no hat, no key piece under rule b either"),
		FMantleHairHatSource::KeyFor(TConstArrayView<FMantleHairHatSource>(), EMantleHairHatAim::UnderHatCards).IsEmpty());

	const auto KeyOf = [](const FMantleHairHatSource& Source) { return FMantleHairHatSource::KeyFor(MakeArrayView(&Source, 1)); };

	FMantleHairHatSource Other = Base;
	Other.MeshHash = TEXT("hat-b");
	TestNotEqual(TEXT("another hat mesh is another key"), KeyOf(Other), BaseKey);

	Other = Base;
	Other.Rule = EMantleHairHatRule::HideAll;
	TestNotEqual(TEXT("another rule is another key"), KeyOf(Other), BaseKey);

	Other = Base;
	Other.Attach.Location = FVector(0.0, 0.0, 1.0);
	TestNotEqual(TEXT("another attach offset is another key"), KeyOf(Other), BaseKey);
	Other.Attach.Location = FVector(0.0, 0.0, 1.0);
	TestEqual(TEXT("and the same offset the same key"), KeyOf(Other), KeyOf(Other));

	Other = Base;
	Other.Attach.Scale = FVector(1.2, 1.2, 1.2);
	TestNotEqual(TEXT("another attach scale is another key"), KeyOf(Other), BaseKey);

	Other = Base;
	Other.Attach.Rotation = FRotator(0.0, 10.0, 0.0);
	TestNotEqual(TEXT("another attach rotation is another key"), KeyOf(Other), BaseKey);

	Other = Base;
	Other.SocketOverride = FMantleCosmeticTransform();
	Other.SocketOverride->Location = FVector(1.0, 0.0, 0.0);
	const FString OverrideKey = KeyOf(Other);
	TestNotEqual(TEXT("a socket override is another key"), OverrideKey, BaseKey);
	Other.SocketOverride->Location = FVector(2.0, 0.0, 0.0);
	TestNotEqual(TEXT("another override is another key"), KeyOf(Other), OverrideKey);

	Other = Base;
	Other.SocketLocal = FTransform(FQuat::Identity, FVector(0.0, 0.0, 3.0));
	TestNotEqual(TEXT("another socket is another key"), KeyOf(Other), BaseKey);

	Other = Base;
	Other.BoneName = FName(TEXT("neck"));
	TestNotEqual(TEXT("another bone is another key"), KeyOf(Other), BaseKey);

	Other = Base;
	Other.SlotId = TEXT("acc_face");
	TestNotEqual(TEXT("another slot is another key"), KeyOf(Other), BaseKey);

	// The cooked bytes are named by the mesh hash: the same hash with other bytes in memory is the same key.
	Other = Base;
	Other.MeshBytes = nullptr;
	TestEqual(TEXT("the key does not read the bytes"), KeyOf(Other), BaseKey);

	// Two hats, in order.
	FMantleHairHatSource Face = Base;
	Face.SlotId = TEXT("acc_face");
	Face.MeshHash = TEXT("mask-a");
	const TArray<FMantleHairHatSource> Both = { Base, Face };
	TestNotEqual(TEXT("a hat and a face item are another key than the hat"), FMantleHairHatSource::KeyFor(Both), BaseKey);

	TestTrue(TEXT("rule text: underIt"), FMantleHairHatSource::RuleFromText(TEXT("underIt")) == EMantleHairHatRule::UnderIt);
	TestTrue(TEXT("rule text: hideAll"), FMantleHairHatSource::RuleFromText(TEXT("hideAll")) == EMantleHairHatRule::HideAll);
	TestTrue(TEXT("rule text: leaveAlone"), FMantleHairHatSource::RuleFromText(TEXT("leaveAlone")) == EMantleHairHatRule::LeaveAlone);
	TestTrue(TEXT("rule text: unknown is underIt"), FMantleHairHatSource::RuleFromText(TEXT("HIDEALL")) == EMantleHairHatRule::UnderIt);
	return true;
}

// The accessory placement the assembler and the hair fit share.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatSourcePlacementTest,
	"Mantle.Runtime.HairHatSource.Placement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatSourcePlacementTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatSourceTestPrivate;

	// The slot table.
	float Dimension = 0.0f, Reach = 0.0f;
	TestTrue(TEXT("acc_hat has bounds"), MantleAccessoryPlacement::FindSlotBounds(TEXT("acc_hat"), Dimension, Reach));
	TestEqual(TEXT("acc_hat longest edge"), Dimension, 60.0f);
	TestEqual(TEXT("acc_hat reach"), Reach, 50.0f);
	TestTrue(TEXT("acc_face has bounds"), MantleAccessoryPlacement::FindSlotBounds(TEXT("acc_face"), Dimension, Reach));
	TestEqual(TEXT("acc_face longest edge"), Dimension, 35.0f);
	TestFalse(TEXT("a garment slot has none"), MantleAccessoryPlacement::FindSlotBounds(TEXT("gloves"), Dimension, Reach));
	TestFalse(TEXT("the slot id is case sensitive"), MantleAccessoryPlacement::FindSlotBounds(TEXT("ACC_HAT"), Dimension, Reach));

	// PlacedRelative is the clamp, the bounds factor and the compose, step by step.
	FMantleCosmeticTransform Attach;
	Attach.Location = FVector(0.0, 0.0, 4.0);
	Attach.Rotation = FRotator(0.0, 30.0, 0.0);
	Attach.Scale = FVector(1.5, 1.5, 1.5);
	const FTransform Socket(FQuat(FRotator(0.0, 0.0, 15.0)), FVector(1.0, 2.0, 3.0));
	FMantleCosmeticTransform Override;
	Override.Location = FVector(5.0, 0.0, 0.0);
	const FMantleCosmeticTransform* const Overrides[] = { nullptr, &Override };
	for (const FMantleCosmeticTransform* Overridden : Overrides)
	{
		for (const FBox3f& Bounds : { FBox3f(FVector3f(-10.0f), FVector3f(10.0f)), FBox3f(FVector3f(-60.0f), FVector3f(60.0f)), FBox3f(ForceInit) })
		{
			float Factor = 0.0f;
			const FTransform Placed = MantleAccessoryPlacement::PlacedRelative(Attach, Bounds, TEXT("acc_hat"), Socket, Overridden, Factor);

			const FTransform Clamped = MantleAccessoryPlacement::ClampedAttach(Attach);
			const float WantedFactor = Bounds.IsValid ? MantleAccessoryPlacement::BoundsClampFactor(Bounds, Clamped.GetScale3D(), 60.0f, 50.0f) : 1.0f;
			FTransform Scaled = Clamped;
			Scaled.SetScale3D(Clamped.GetScale3D() * WantedFactor);
			const FTransform Wanted = MantleAccessoryPlacement::ComposeRelative(Scaled, Socket, Overridden);
			TestEqual(TEXT("the factor is the bounds factor"), Factor, WantedFactor);
			TestTrue(TEXT("the placement is the composed one"), Placed.Equals(Wanted, 1.0e-6));
		}
	}
	float BigFactor = 1.0f;
	MantleAccessoryPlacement::PlacedRelative(Attach, FBox3f(FVector3f(-60.0f), FVector3f(60.0f)), TEXT("acc_hat"), Socket, nullptr, BigFactor);
	TestTrue(TEXT("a hat far past the slot's bounds is scaled down"), BigFactor < 1.0f);
	float NoSlot = 0.0f;
	MantleAccessoryPlacement::PlacedRelative(Attach, FBox3f(FVector3f(-60.0f), FVector3f(60.0f)), TEXT("gloves"), Socket, nullptr, NoSlot);
	TestEqual(TEXT("a slot without bounds is not scaled"), NoSlot, 1.0f);

	// A bone's place in the reference pose: its own pose, then each parent's.
	FMantleSkinnedMesh Body;
	Body.Bones.Add({ FName(TEXT("root")), INDEX_NONE });
	Body.Bones.Add({ FName(TEXT("spine")), 0 });
	Body.Bones.Add({ FName(TEXT("head")), 1 });
	const FTransform RootPose(FQuat(FRotator(0.0, 0.0, 0.0)), FVector(0.0, 0.0, 5.0));
	const FTransform SpinePose(FQuat(FRotator(0.0, 40.0, 25.0)), FVector(10.0, 0.0, 0.0));
	const FTransform HeadPose(FQuat(FRotator(10.0, 0.0, 0.0)), FVector(0.0, 0.0, 60.0));
	Body.ReferencePose = { RootPose, SpinePose, HeadPose };
	FTransform HeadInComponent;
	TestTrue(TEXT("the head has a place"), FMantleHairHatSource::BoneComponentTransform(Body, FName(TEXT("head")), HeadInComponent));
	const FVector Probe(3.0, -2.0, 7.0);
	TestTrue(TEXT("a point in the head's space goes through the head, the spine, then the root"),
		HeadInComponent.TransformPosition(Probe).Equals(RootPose.TransformPosition(SpinePose.TransformPosition(HeadPose.TransformPosition(Probe))), 1.0e-6));
	TestFalse(TEXT("a bone the body lacks has none"), FMantleHairHatSource::BoneComponentTransform(Body, FName(TEXT("tail")), HeadInComponent));
	TestFalse(TEXT("no bone name has none"), FMantleHairHatSource::BoneComponentTransform(Body, NAME_None, HeadInComponent));
	FMantleSkinnedMesh NoPose = Body;
	NoPose.ReferencePose.Reset();
	TestFalse(TEXT("a body without a reference pose has none"), FMantleHairHatSource::BoneComponentTransform(NoPose, FName(TEXT("head")), HeadInComponent));

	// The hat in the hair's space: attach, then the socket (or the avatar's override of it), then the bone.
	FMantleHairHatSource Source;
	Source.SlotId = TEXT("acc_hat");
	Source.BoneName = FName(TEXT("head"));
	Source.SocketLocal = FTransform(FQuat::Identity, FVector(1.0, 2.0, 3.0));
	Source.Attach.Location = FVector(0.0, 0.0, 5.0);
	FBox3f Small(FVector3f(-5.0f), FVector3f(5.0f));
	FMantleSkinnedMesh Flat = MakeBody();
	FTransform ToHair;
	FString Error;
	TestTrue(TEXT("the placement is found"), Source.HatToHair(Flat, Small, ToHair, Error));
	TestTrue(TEXT("on the socket: the point moves by attach and socket"),
		ToHair.TransformPosition(FVector(0.0, 0.0, 0.0)).Equals(FVector(1.0, 2.0, 8.0 + HeadZ), 1.0e-6));

	Source.SocketOverride = FMantleCosmeticTransform();
	Source.SocketOverride->Location = FVector(4.0, 0.0, 0.0);
	TestTrue(TEXT("the placement with an override is found"), Source.HatToHair(Flat, Small, ToHair, Error));
	TestTrue(TEXT("on the overridden socket: attach, then the override's place"),
		ToHair.TransformPosition(FVector(0.0, 0.0, 0.0)).Equals(FVector(4.0, 0.0, 5.0 + HeadZ), 1.0e-6));

	Source.BoneName = FName(TEXT("tail"));
	TestFalse(TEXT("a bone the body lacks is refused"), Source.HatToHair(Flat, Small, ToHair, Error));
	TestFalse(TEXT("with a reason"), Error.IsEmpty());
	return true;
}

// The step on a hair: pulled in, cut out, left alone, for the scalp hair, the facial hair, and when it cannot run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatSourceApplyTest,
	"Mantle.Runtime.HairHatSource.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatSourceApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatSourceTestPrivate;
	const FMantleSkinnedMesh Body = MakeBody();
	const FMantleSkinnedMesh Face = MakeFace();
	const FMantleHairHatSource Source = MakeSource();
	const FMantleSkinnedMesh Hair = MakeHair(FourTriangles(), 2);
	TestEqual(TEXT("the test hair is a mesh"), ValidityOf(Hair), FString());

	// The decoded hat is the dome, whose bounds are its own.
	TArray<FVector3f> HatP;
	TArray<uint32> HatI;
	FBox3f HatBounds(ForceInit);
	FString Error;
	TestTrue(TEXT("the hat's bytes read"), Source.ReadMesh(HatP, HatI, HatBounds, Error));
	TestTrue(TEXT("with its triangles"), HatI.Num() > 0 && HatP.Num() > 0);
	TestNearlyEqual(TEXT("and its bounds"), HatBounds.Max.Z, 10.0f, 1.0e-3f);

	// Scalp hair: cut and pulled on every LOD.
	{
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&Source, 1), Body, Face, false, Fitted, Stats, Error));
		TestTrue(TEXT("and says so"), Stats.bApplied);
		TestEqual(TEXT("one hat took part"), Stats.NumHats, 1);
		TestTrue(TEXT("the hat's triangles were tested against"), Stats.HatTriangles > 0);
		TestEqual(TEXT("one corner pulled in, on each of two LODs"), Stats.PointsPulled, 2);
		TestEqual(TEXT("one triangle cut, on each of two LODs"), Stats.TrianglesDropped, 2);
		TestTrue(TEXT("the time is taken"), Stats.Ms >= Stats.FitMs && Stats.FitMs >= 0.0);
		TestEqual(TEXT("the hair is still a mesh"), ValidityOf(Fitted), FString());
		for (int32 LODIndex = 0; LODIndex < Fitted.LODs.Num(); ++LODIndex)
		{
			const FMantleSkinnedMeshLOD& Before = Hair.LODs[LODIndex];
			const FMantleSkinnedMeshLOD& After = Fitted.LODs[LODIndex];
			TestEqual(TEXT("three triangles stay"), After.Indices.Num(), 9);
			TestEqual(TEXT("the section holds them"), After.Sections[0].IndexCount, 9);
			// The kept triangles keep their corners: 0 and 3 untouched, the second pulled to 0.5 cm under the dome on its ray.
			for (const int32 V : { 0, 1, 2, 4, 5, 9, 10, 11 })
			{
				TestTrue(*FString::Printf(TEXT("vertex %d is where it was"), V), After.Positions[V] == Before.Positions[V]);
			}
			const FVector3f Poking = Before.Positions[3];
			const float WasAt = (Poking - HeadCentre()).Size();
			const float HatAt = HatAlong(30.0f, 100.0f);
			TestNearlyEqual(TEXT("the poking point was 0.2 cm beyond the dome"), WasAt, HatAt + 0.2f, 1.0e-2f);
			TestNearlyEqual(TEXT("and is 0.5 cm under it now"), (After.Positions[3] - HeadCentre()).Size(), HatAt - 0.5f, 1.0e-2f);
			TestTrue(TEXT("on the same ray"), (After.Positions[3] - HeadCentre()).GetSafeNormal().Equals((Poking - HeadCentre()).GetSafeNormal(), 1.0e-4f));
		}
	}

	// Facial hair: only cut. The triangle with one corner poking stays where it was.
	{
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("the step runs on facial hair"), FMantleHairHatSource::Apply(MakeArrayView(&Source, 1), Body, Face, true, Fitted, Stats, Error));
		TestEqual(TEXT("nothing is pulled"), Stats.PointsPulled, 0);
		TestEqual(TEXT("the wholly poking triangle is cut on each LOD"), Stats.TrianglesDropped, 2);
		TestTrue(TEXT("the poking corner stays where it was"), Fitted.LODs[0].Positions[3] == Hair.LODs[0].Positions[3]);
		TestEqual(TEXT("the hair is still a mesh"), ValidityOf(Fitted), FString());
	}

	// No hat, or a hat left alone: nothing changes.
	{
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("no hat: the step runs"), FMantleHairHatSource::Apply(TConstArrayView<FMantleHairHatSource>(), Body, Face, false, Fitted, Stats, Error));
		TestFalse(TEXT("and does nothing"), Stats.bApplied);
		TestTrue(TEXT("the hair is as it was"), Fitted.LODs[0].Positions == Hair.LODs[0].Positions && Fitted.LODs[0].Indices == Hair.LODs[0].Indices);

		FMantleHairHatSource Alone = Source;
		Alone.Rule = EMantleHairHatRule::LeaveAlone;
		TestTrue(TEXT("leaveAlone: the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&Alone, 1), Body, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("with no hat taking part"), Stats.NumHats, 0);
		TestTrue(TEXT("the hair is as it was"), Fitted.LODs[0].Positions == Hair.LODs[0].Positions && Fitted.LODs[0].Indices == Hair.LODs[0].Indices);
	}

	// A hat that takes every triangle leaves one tiny triangle inside the head, so the LOD is still a mesh.
	{
		const TArray<FVector3f> All = { OnRay(40.0f, 200.0f, HatAlong(40.0f, 200.0f) + 1.0f), OnRay(40.0f, 220.0f, HatAlong(40.0f, 220.0f) + 1.0f),
			OnRay(60.0f, 200.0f, HatAlong(60.0f, 200.0f) + 1.0f) };
		FMantleSkinnedMesh Fitted = MakeHair(All);
		FMantleHairHatStats Stats;
		TestTrue(TEXT("every triangle cut: the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&Source, 1), Body, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("one triangle was cut"), Stats.TrianglesDropped, 1);
		TestEqual(TEXT("one triangle is left"), Fitted.LODs[0].Indices.Num(), 3);
		TestEqual(TEXT("the hair is still a mesh"), ValidityOf(Fitted), FString());
		for (const FVector3f& Corner : Fitted.LODs[0].Positions)
		{
			TestTrue(TEXT("each corner is inside the head, by its centre"), (Corner - HeadCentre()).Size() < 0.1f);
		}
	}

	// A hat that cannot be read, or has no bone: the hair is left as it was, with the reason.
	{
		FMantleHairHatSource Broken = Source;
		Broken.MeshBytes = nullptr;
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestFalse(TEXT("a hat with no bytes: the step reports it"), FMantleHairHatSource::Apply(MakeArrayView(&Broken, 1), Body, Face, false, Fitted, Stats, Error));
		TestFalse(TEXT("with a reason"), Stats.Error.IsEmpty());
		TestFalse(TEXT("and did not apply"), Stats.bApplied);
		TestTrue(TEXT("the hair is as it was"), Fitted.LODs[0].Positions == Hair.LODs[0].Positions && Fitted.LODs[0].Indices == Hair.LODs[0].Indices);

		Broken = Source;
		Broken.BoneName = FName(TEXT("tail"));
		TestFalse(TEXT("a hat on a bone the body lacks: the step reports it"), FMantleHairHatSource::Apply(MakeArrayView(&Broken, 1), Body, Face, false, Fitted, Stats, Error));

		FMantleSkinnedMesh NoSkin;
		NoSkin.LODs.AddDefaulted();
		TestFalse(TEXT("a face with no head skin: the step reports it"), FMantleHairHatSource::Apply(MakeArrayView(&Source, 1), Body, NoSkin, false, Fitted, Stats, Error));
		TestTrue(TEXT("the hair is as it was"), Fitted.LODs[0].Positions == Hair.LODs[0].Positions && Fitted.LODs[0].Indices == Hair.LODs[0].Indices);
	}

	// Where the hat sits decides what is under it: the same hat on a head bone 30 cm lower leaves the hair above it alone.
	{
		FMantleSkinnedMesh LowBody = Body;
		LowBody.ReferencePose[1] = FTransform(FQuat::Identity, FVector(0.0, 0.0, HeadZ - 30.0));
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("the step runs on a lower head bone"), FMantleHairHatSource::Apply(MakeArrayView(&Source, 1), LowBody, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("a hat 30 cm below the hair pulls nothing"), Stats.PointsPulled, 0);
	}
	return true;
}

// The hood of fitted outerwear (#389, H6): the triangles whose three corners are each weighted 0.5 or more to the head bone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHoodExtractTest,
	"Mantle.Runtime.HairHatSource.HoodExtract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHoodExtractTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatSourceTestPrivate;
	const FMantleSkinnedMesh Jacket = MakeJacket();
	TestEqual(TEXT("the test jacket is a mesh"), ValidityOf(Jacket), FString());

	FMantleHairHood Hood;
	TestTrue(TEXT("a jacket with head-weighted triangles has a hood"), FMantleHairHood::Extract(Jacket, Hood));
	// Kept: 0, 2, 3 and 5. Dropped: 1 (one corner at 127/255, just under a half) and 4 (the root's).
	TestEqual(TEXT("four triangles are the hood"), Hood.NumTriangles(), 4);
	TestEqual(TEXT("the vertices are the ones the kept triangles use, shared where they share"), Hood.Positions.Num(), 10);
	const FMantleSkinnedMeshLOD& LOD = Jacket.LODs[0];
	// The hood's triangles, as positions, in the order the jacket lists them.
	const TArray<int32> KeptCorners = { 0, 1, 2,  6, 7, 8,  9, 10, 11,  0, 2, 15 };
	TestEqual(TEXT("the index list is whole triangles"), Hood.Indices.Num(), KeptCorners.Num());
	for (int32 Corner = 0; Corner < KeptCorners.Num() && Corner < Hood.Indices.Num(); ++Corner)
	{
		TestTrue(*FString::Printf(TEXT("corner %d is the jacket's vertex %d"), Corner, KeptCorners[Corner]),
			Hood.Positions[Hood.Indices[Corner]] == LOD.Positions[KeptCorners[Corner]]);
	}
	TestTrue(TEXT("a shared vertex is used once"), Hood.Indices[0] == Hood.Indices[9] && Hood.Indices[2] == Hood.Indices[10]);
	TestEqual(TEXT("the byte count follows"), Hood.Bytes(), static_cast<int64>(Hood.Positions.GetAllocatedSize() + Hood.Indices.GetAllocatedSize()));

	// The weight limit is a half of 255: 127 is under, 128 is over (the jacket's triangles 1 and 2).
	TestTrue(TEXT("the limit is a half"), FMantleHairHood::MinHeadWeight == 0.5f);
	const TSharedPtr<const FMantleHairHood, ESPMode::ThreadSafe> Shared = FMantleHairHood::Make(Jacket);
	TestTrue(TEXT("Make gives the shared hood"), Shared.IsValid() && Shared->NumTriangles() == 4);

	// A jacket without a hood: every corner on the root bone.
	FMantleSkinnedMesh Hoodless = Jacket;
	FMantleSkinnedMeshLOD& HoodlessLOD = Hoodless.LODs[0];
	for (int32 Vertex = 0; Vertex < HoodlessLOD.NumVertices(); ++Vertex)
	{
		HoodlessLOD.InfluenceBones[Vertex * 3] = 0;
		HoodlessLOD.InfluenceWeights[Vertex * 3] = 255;
		HoodlessLOD.InfluenceBones[Vertex * 3 + 1] = 0;
		HoodlessLOD.InfluenceWeights[Vertex * 3 + 1] = 0;
		HoodlessLOD.InfluenceBones[Vertex * 3 + 2] = 0;
		HoodlessLOD.InfluenceWeights[Vertex * 3 + 2] = 0;
	}
	FMantleHairHood None;
	TestFalse(TEXT("nothing on the head: no hood"), FMantleHairHood::Extract(Hoodless, None));
	TestTrue(TEXT("and the result is empty"), None.Positions.IsEmpty() && None.Indices.IsEmpty());
	TestFalse(TEXT("Make gives none"), FMantleHairHood::Make(Hoodless).IsValid());

	// A triangle with one corner under the limit is not part of a hood, however the other two are weighted.
	FMantleSkinnedMesh OneShort = Jacket;
	OneShort.LODs[0].Indices = { 3, 4, 5 };
	TestFalse(TEXT("a triangle with a corner at 127/255 is not a hood"), FMantleHairHood::Extract(OneShort, None));
	OneShort.LODs[0].Indices = { 6, 7, 8 };
	TestTrue(TEXT("with that corner at 128/255 it is"), FMantleHairHood::Extract(OneShort, None));

	// A body without a head bone, a mesh without a LOD, and skin data that does not match its vertices: no hood, no crash.
	FMantleSkinnedMesh NoHead = Jacket;
	NoHead.Bones[1].Name = FName(TEXT("neck"));
	TestFalse(TEXT("no head bone: no hood"), FMantleHairHood::Extract(NoHead, None));
	TestFalse(TEXT("no LOD: no hood"), FMantleHairHood::Extract(FMantleSkinnedMesh(), None));
	FMantleSkinnedMesh Short = Jacket;
	Short.LODs[0].InfluenceWeights.SetNum(Short.LODs[0].InfluenceWeights.Num() - 1);
	TestFalse(TEXT("skin data that does not match the vertices: no hood"), FMantleHairHood::Extract(Short, None));
	FMantleSkinnedMesh OutOfRange = Jacket;
	OutOfRange.LODs[0].Indices = { 0, 1, 99 };
	TestFalse(TEXT("an index out of range is skipped"), FMantleHairHood::Extract(OutOfRange, None));

	// Only LOD0 is read.
	FMantleSkinnedMesh TwoLODs = Jacket;
	TwoLODs.LODs.Add(Jacket.LODs[0]);
	TwoLODs.LODs[1].Indices = { 3, 4, 5 };
	TestTrue(TEXT("a second LOD changes nothing"), FMantleHairHood::Extract(TwoLODs, Hood) && Hood.NumTriangles() == 4);
	return true;
}

// The step with a hood on the sphere head: the same dome as a hat, tested the same way, and the key's hood part.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHoodApplyTest,
	"Mantle.Runtime.HairHatSource.HoodApply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHoodApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatSourceTestPrivate;
	const FMantleSkinnedMesh Body = MakeBody();
	const FMantleSkinnedMesh Face = MakeFace();
	const FMantleHairHatSource Hood = MakeHoodSource();
	const FMantleSkinnedMesh Hair = MakeHair(FourTriangles(), 2);
	FString Error;

	// The key: the outerwear's fit and the rule, in the same segment as a hat's; no placement.
	const auto KeyOf = [](const FMantleHairHatSource& Source) { return FMantleHairHatSource::KeyFor(MakeArrayView(&Source, 1)); };
	const FString HoodKey = KeyOf(Hood);
	TestTrue(TEXT("the key is the hat segment"), HoodKey.StartsWith(FString::Printf(TEXT(":hathair-v%d["), FMantleHairHatSource::RuleVersion)));
	TestTrue(TEXT("naming the outerwear and its fit"), HoodKey.Contains(TEXT("outerwear=jacket-a/underIt/hood")));
	FMantleHairHatSource Other = Hood;
	Other.MeshHash = TEXT("jacket-b");
	TestNotEqual(TEXT("another fit is another key"), KeyOf(Other), HoodKey);
	Other = Hood;
	Other.Rule = EMantleHairHatRule::HideAll;
	TestNotEqual(TEXT("another rule is another key"), KeyOf(Other), HoodKey);
	const FMantleHairHatSource Hat = MakeSource();
	TestNotEqual(TEXT("a hood is not a hat"), KeyOf(Hat), HoodKey);
	const TArray<FMantleHairHatSource> HatAndHood = { Hat, Hood };
	TestNotEqual(TEXT("a hat and a hood are another key than the hat"), FMantleHairHatSource::KeyFor(HatAndHood), KeyOf(Hat));

	// Scalp hair under a hood shaped like the dome: what the hat does, on every LOD.
	{
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&Hood, 1), Body, Face, false, Fitted, Stats, Error));
		TestTrue(TEXT("and says so"), Stats.bApplied);
		TestEqual(TEXT("one hood took part"), Stats.NumHoods, 1);
		TestEqual(TEXT("and no hat"), Stats.NumHats, 0);
		TestTrue(TEXT("its triangles were tested against"), Stats.HatTriangles > 0);
		TestEqual(TEXT("one corner pulled in, on each of two LODs"), Stats.PointsPulled, 2);
		TestEqual(TEXT("one triangle cut, on each of two LODs"), Stats.TrianglesDropped, 2);
		TestEqual(TEXT("the hair is still a mesh"), ValidityOf(Fitted), FString());
		for (int32 LODIndex = 0; LODIndex < Fitted.LODs.Num(); ++LODIndex)
		{
			const FMantleSkinnedMeshLOD& Before = Hair.LODs[LODIndex];
			const FMantleSkinnedMeshLOD& After = Fitted.LODs[LODIndex];
			TestEqual(TEXT("three triangles stay"), After.Indices.Num(), 9);
			for (const int32 V : { 0, 1, 2, 4, 5, 9, 10, 11 })
			{
				TestTrue(*FString::Printf(TEXT("vertex %d is where it was"), V), After.Positions[V] == Before.Positions[V]);
			}
			TestNearlyEqual(TEXT("the poking point is 0.5 cm under the hood"), (After.Positions[3] - HeadCentre()).Size(), HatAlong(30.0f, 100.0f) - 0.5f, 1.0e-2f);
		}
	}

	// Facial hair: only cut.
	{
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("the step runs on facial hair"), FMantleHairHatSource::Apply(MakeArrayView(&Hood, 1), Body, Face, true, Fitted, Stats, Error));
		TestEqual(TEXT("nothing is pulled"), Stats.PointsPulled, 0);
		TestEqual(TEXT("the wholly poking triangle is cut on each LOD"), Stats.TrianglesDropped, 2);
	}

	// leaveAlone: nothing takes part.
	{
		FMantleHairHatSource Alone = Hood;
		Alone.Rule = EMantleHairHatRule::LeaveAlone;
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("leaveAlone: the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&Alone, 1), Body, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("with no hood taking part"), Stats.NumHoods, 0);
		TestTrue(TEXT("the hair is as it was"), Fitted.LODs[0].Positions == Hair.LODs[0].Positions && Fitted.LODs[0].Indices == Hair.LODs[0].Indices);
	}

	// A hat and a hood together: both count, and the triangles of both are what the hair is tested against.
	{
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("a hat and a hood: the step runs"), FMantleHairHatSource::Apply(HatAndHood, Body, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("one hat"), Stats.NumHats, 1);
		TestEqual(TEXT("one hood"), Stats.NumHoods, 1);
		FMantleSkinnedMesh HatOnly = Hair;
		FMantleHairHatStats HatStats;
		TestTrue(TEXT("the hat alone: the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&Hat, 1), Body, Face, false, HatOnly, HatStats, Error));
		TestTrue(TEXT("the hood counts its triangles on top of the hat's"), Stats.HatTriangles > HatStats.HatTriangles);
		TestEqual(TEXT("the two domes lie on each other, so the hair is cut the same"), Stats.TrianglesDropped, HatStats.TrianglesDropped);
	}

	// A hood 30 cm lower than the hair leaves the hair alone: it is used where it is (already in the hair's space), whatever the head bone says.
	{
		TSharedRef<FMantleHairHood, ESPMode::ThreadSafe> Low = MakeShared<FMantleHairHood, ESPMode::ThreadSafe>();
		PlacedDome(Low->Positions, Low->Indices);
		for (FVector3f& Position : Low->Positions)
		{
			Position.Z -= 30.0f;
		}
		FMantleHairHatSource LowHood = Hood;
		LowHood.Hood = Low;
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("a hood 30 cm lower: the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&LowHood, 1), Body, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("it pulls nothing"), Stats.PointsPulled, 0);
	}

	// Hood triangles that name a vertex it lacks are skipped, and a hood with no triangles takes no part in the fit.
	{
		TSharedRef<FMantleHairHood, ESPMode::ThreadSafe> Broken = MakeShared<FMantleHairHood, ESPMode::ThreadSafe>();
		Broken->Positions = { FVector3f(0.0f, 0.0f, 0.0f) };
		Broken->Indices = { 0, 1, 2 };
		FMantleHairHatSource BrokenHood = Hood;
		BrokenHood.Hood = Broken;
		FMantleSkinnedMesh Fitted = Hair;
		FMantleHairHatStats Stats;
		TestTrue(TEXT("a hood with a bad triangle: the step runs"), FMantleHairHatSource::Apply(MakeArrayView(&BrokenHood, 1), Body, Face, false, Fitted, Stats, Error));
		TestEqual(TEXT("no triangle was usable"), Stats.HatTriangles, 0);
		TestTrue(TEXT("the hair is as it was"), Fitted.LODs[0].Positions == Hair.LODs[0].Positions && Fitted.LODs[0].Indices == Hair.LODs[0].Indices);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
