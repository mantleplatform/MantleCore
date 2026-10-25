// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisBuilder.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "MantleChassisSpec.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisBuilderPrivate
{
	/** Sides of a wheel's grey cylinder. */
	constexpr int32 WheelSides = 12;

	/** Metres a texture repeat covers on the grey shapes, as UV per cm. */
	constexpr float UVPerCm = 0.01f;

	/** The section's material name: the body is drawn with Mantle's default surface. */
	const TCHAR* const SectionName = TEXT("chassis");

	/** A propeller's two blades, cm at scale 1: how thick along X and how wide each is (half of both, from the hub), and a twin's nacelle's half-width. */
	constexpr double PropellerHalfThickness = 3.0;
	constexpr double PropellerHalfWidth = 12.0;
	constexpr double NacelleHalfWidth = 30.0;

	bool Refuse(FString& OutError, const FString& Why)
	{
		OutError = Why;
		return false;
	}

	/** Grey geometry into one LOD: flat-shaded faces, every vertex skinned wholly to one bone. */
	struct FShapeBuilder
	{
		FMantleSkinnedMeshLOD& LOD;

		explicit FShapeBuilder(FMantleSkinnedMeshLOD& InLOD) : LOD(InLOD) {}

		int32 AddVertex(const FVector3f& Position, const FVector3f& Normal, const FVector2f& UV, const int32 Bone)
		{
			const int32 Index = LOD.Positions.Num();
			LOD.Positions.Add(Position);
			LOD.Normals.Add(Normal);
			LOD.UVs.Add(UV);
			for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
			{
				// A slot with no weight still names a real bone: the root.
				LOD.InfluenceBones.Add(static_cast<uint16>(Slot == 0 ? Bone : 0));
				LOD.InfluenceWeights.Add(static_cast<uint8>(Slot == 0 ? 255 : 0));
			}
			return Index;
		}

		/** A triangle, wound so its front faces where the first corner's normal points (the engine's front is (C - A) ^ (B - A)). */
		void AddTriangle(const int32 A, const int32 B, const int32 C)
		{
			const FVector3f Facing = FVector3f::CrossProduct(LOD.Positions[C] - LOD.Positions[A], LOD.Positions[B] - LOD.Positions[A]);
			const bool bFlip = FVector3f::DotProduct(Facing, LOD.Normals[A]) < 0.0f;
			LOD.Indices.Add(static_cast<uint32>(A));
			LOD.Indices.Add(static_cast<uint32>(bFlip ? C : B));
			LOD.Indices.Add(static_cast<uint32>(bFlip ? B : C));
		}

		/** A flat quad of four consecutive corners. */
		void AddQuad(const FVector3f Corners[4], const FVector3f& Normal, const FVector2f Uvs[4], const int32 Bone)
		{
			int32 Index[4];
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				Index[Corner] = AddVertex(Corners[Corner], Normal, Uvs[Corner], Bone);
			}
			AddTriangle(Index[0], Index[1], Index[2]);
			AddTriangle(Index[0], Index[2], Index[3]);
		}

		/** An axis-aligned box. */
		void AddBox(const FVector3f& Min, const FVector3f& Max, const int32 Bone)
		{
			const FVector3f Centre = 0.5f * (Min + Max);
			const FVector3f Half = 0.5f * (Max - Min);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const int32 U = (Axis + 1) % 3;
				const int32 V = (Axis + 2) % 3;
				for (const float Sign : { -1.0f, 1.0f })
				{
					FVector3f Normal = FVector3f::ZeroVector;
					Normal[Axis] = Sign;
					FVector3f Corners[4];
					FVector2f Uvs[4];
					const float CornerU[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
					const float CornerV[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
					for (int32 Corner = 0; Corner < 4; ++Corner)
					{
						Corners[Corner] = Centre;
						Corners[Corner][Axis] += Sign * Half[Axis];
						Corners[Corner][U] += CornerU[Corner] * Half[U];
						Corners[Corner][V] += CornerV[Corner] * Half[V];
						Uvs[Corner] = FVector2f(Corners[Corner][U], Corners[Corner][V]) * UVPerCm;
					}
					AddQuad(Corners, Normal, Uvs, Bone);
				}
			}
		}

		/** A cylinder about the Y axis (a wheel): Radius in X and Z, Width along Y, centred on Centre. */
		void AddWheel(const FVector3f& Centre, const float Radius, const float Width, const int32 Bone)
		{
			const float HalfWidth = 0.5f * Width;
			auto Rim = [&Centre, Radius](const int32 Step, const float Y)
			{
				const float Angle = UE_TWO_PI * static_cast<float>(Step) / static_cast<float>(WheelSides);
				return FVector3f(Centre.X + Radius * FMath::Cos(Angle), Centre.Y + Y, Centre.Z + Radius * FMath::Sin(Angle));
			};
			for (int32 Step = 0; Step < WheelSides; ++Step)
			{
				const float Middle = UE_TWO_PI * (static_cast<float>(Step) + 0.5f) / static_cast<float>(WheelSides);
				const FVector3f Normal(FMath::Cos(Middle), 0.0f, FMath::Sin(Middle));
				const FVector3f Corners[4] = { Rim(Step, -HalfWidth), Rim(Step + 1, -HalfWidth), Rim(Step + 1, HalfWidth), Rim(Step, HalfWidth) };
				const float Across = 2.0f * Radius * UVPerCm;
				const FVector2f Uvs[4] = { FVector2f(0.0f, 0.0f), FVector2f(Across, 0.0f), FVector2f(Across, Width * UVPerCm), FVector2f(0.0f, Width * UVPerCm) };
				AddQuad(Corners, Normal, Uvs, Bone);
			}
			// The two faces, each a fan about its middle.
			for (const float Sign : { -1.0f, 1.0f })
			{
				const FVector3f Normal(0.0f, Sign, 0.0f);
				const int32 Hub = AddVertex(Centre + FVector3f(0.0f, Sign * HalfWidth, 0.0f), Normal, FVector2f(0.5f, 0.5f), Bone);
				int32 Ring[WheelSides];
				for (int32 Step = 0; Step < WheelSides; ++Step)
				{
					const float Angle = UE_TWO_PI * static_cast<float>(Step) / static_cast<float>(WheelSides);
					Ring[Step] = AddVertex(Rim(Step, Sign * HalfWidth), Normal, FVector2f(0.5f + 0.5f * FMath::Cos(Angle), 0.5f + 0.5f * FMath::Sin(Angle)), Bone);
				}
				for (int32 Step = 0; Step < WheelSides; ++Step)
				{
					AddTriangle(Hub, Ring[Step], Ring[(Step + 1) % WheelSides]);
				}
			}
		}
	};
}

FName FMantleVehicleChassisBuilder::RootBoneName()
{
	return FName(TEXT("root"));
}

FName FMantleVehicleChassisBuilder::FrontForkBoneName()
{
	return FName(TEXT("fork_f"));
}

FName FMantleVehicleChassisBuilder::AileronBoneName(const bool bRight)
{
	return FName(bRight ? TEXT("aileron_r") : TEXT("aileron_l"));
}

FName FMantleVehicleChassisBuilder::ElevatorBoneName()
{
	return FName(TEXT("elevator"));
}

FName FMantleVehicleChassisBuilder::RudderBoneName()
{
	return FName(TEXT("rudder"));
}

FName FMantleVehicleChassisBuilder::PropellerBoneName(const int32 Engine, const int32 EngineCount)
{
	return FName(EngineCount < 2 ? TEXT("propeller") : (Engine == 0 ? TEXT("propeller_l") : TEXT("propeller_r")));
}

FName FMantleVehicleChassisBuilder::NozzleBoneName(const int32 Engine, const int32 EngineCount)
{
	return FName(EngineCount < 2 ? TEXT("nozzle") : (Engine == 0 ? TEXT("nozzle_l") : TEXT("nozzle_r")));
}

FName FMantleVehicleChassisBuilder::EngineBoneName(const FMantleChassisAircraft& Aircraft, const int32 Engine)
{
	return Aircraft.bPropeller ? PropellerBoneName(Engine, Aircraft.EngineCount) : NozzleBoneName(Engine, Aircraft.EngineCount);
}

FName FMantleVehicleChassisBuilder::MainRotorBoneName()
{
	return FName(TEXT("main_rotor"));
}

FName FMantleVehicleChassisBuilder::TailRotorBoneName()
{
	return FName(TEXT("tail_rotor"));
}

FString FMantleVehicleChassisBuilder::PresetDirectory()
{
#if MANTLE_RUNTIME_IN_GAME
	// The game's copy of this module (MantleGameRuntime) runs without the plugin: the game stages its own presets.
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("Resources"), TEXT("VehiclePresets"));
#else
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("VehiclePresets")) : FString();
#endif
}

TArray<FString> FMantleVehicleChassisBuilder::PresetIds()
{
	TArray<FString> Ids;
	const FString Directory = PresetDirectory();
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().FindFiles(Ids, *FPaths::Combine(Directory, TEXT("*.json")), /*Files*/ true, /*Directories*/ false);
		for (FString& Id : Ids)
		{
			Id = FPaths::GetBaseFilename(Id);
		}
		Ids.Sort();
	}
	return Ids;
}

bool FMantleVehicleChassisBuilder::LoadPresetText(const FString& PresetId, FString& OutText, FString& OutError)
{
	// The id is a file's name: lowercase letters, digits and dashes, so it can name nothing outside the directory.
	if (!FMantleChassisSpec::IsPresetId(PresetId))
	{
		return MantleChassisBuilderPrivate::Refuse(OutError, FString::Printf(TEXT("'%s' is not a vehicle preset id"), *PresetId));
	}
	const FString Path = FPaths::Combine(PresetDirectory(), PresetId + TEXT(".json"));
	if (!FFileHelper::LoadFileToString(OutText, *Path))
	{
		return MantleChassisBuilderPrivate::Refuse(OutError, FString::Printf(TEXT("the vehicle preset file %s could not be read"), *Path));
	}
	return true;
}

bool FMantleVehicleChassisBuilder::ReadReference(const FString& PresetJsonText, FMantleVehicleChassisReference& OutReference, FString& OutError)
{
	FMantleChassisSpec Spec;
	if (!FMantleChassisSpec::Parse(PresetJsonText, Spec, OutError))
	{
		return false;
	}
	OutReference = ReferenceOf(Spec);
	return true;
}

FMantleVehicleChassisReference FMantleVehicleChassisBuilder::ReferenceOf(const FMantleChassisSpec& Spec)
{
	FMantleVehicleChassisReference Reference;
	Reference.Wheelbase = Spec.Wheelbase;
	for (const FMantleChassisAxle& Axle : Spec.Axles)
	{
		Reference.AxlePositions.Add(Axle.Position);
	}
	Reference.Track = Spec.Track;
	Reference.WheelRadius = Spec.WheelRadius;
	Reference.WheelWidth = Spec.WheelWidth;
	Reference.BodyLength = Spec.Length;
	Reference.BodyWidth = Spec.Width;
	Reference.BodyHeight = Spec.Height;
	Reference.GroundClearance = Spec.Clearance;
	Reference.CenterOfMass = Spec.CenterOfMass;
	Reference.Seat = Spec.Seat;
	Reference.SteeringWheelHub = Spec.Hub;
	Reference.SteeringWheelRadius = Spec.RimRadius;
	Reference.SteeringWheelPitch = FMantleChassisSpec::SteeringWheelPitch;
	Reference.bPassenger = Spec.bPassenger;
	Reference.bCentreline = Spec.bCentreline;
	Reference.DriverSeat = Spec.DriverSeat;
	Reference.bSteeringWheel = Spec.bSteeringWheel;
	Reference.ForkBone = Spec.ForkBone;
	Reference.Rider = Spec.Rider;
	return Reference;
}

bool FMantleVehicleChassisBuilder::LoadSpec(const FString& SelectorText, FMantleChassisSpec& OutSpec, FString& OutError)
{
	FString PresetId;
	double Scale = 1.0;
	FString Text;
	FMantleChassisSpec Preset;
	if (!FMantleChassisSpec::ParseSelector(SelectorText, PresetId, Scale, OutError) || !LoadPresetText(PresetId, Text, OutError)
		|| !FMantleChassisSpec::Parse(Text, Preset, OutError))
	{
		return false;
	}
	OutSpec = FMantleChassisSpec::Scaled(Preset, Scale);
	return true;
}

bool FMantleVehicleChassisBuilder::MakeDocument(const FString& PresetJsonText, FString& OutDocument, FString& OutError)
{
	FMantleChassisSpec Spec;
	if (!FMantleChassisSpec::Parse(PresetJsonText, Spec, OutError))
	{
		return false;
	}
	OutDocument = FMantleChassisSpec::ComposeDocument(Spec);
	return true;
}

bool FMantleVehicleChassisBuilder::MakeAsset(const FString& PresetJsonText, FMantleVehicleAsset& OutAsset, FString& OutError)
{
	FMantleChassisSpec Spec;
	if (!FMantleChassisSpec::Parse(PresetJsonText, Spec, OutError))
	{
		return false;
	}
	return MakeAsset(Spec, OutAsset, OutError);
}

bool FMantleVehicleChassisBuilder::MakeAsset(const FMantleChassisSpec& Spec, FMantleVehicleAsset& OutAsset, FString& OutError)
{
	using namespace MantleChassisBuilderPrivate;
	const FTCHARToUTF8 Document(*FMantleChassisSpec::ComposeDocument(Spec));
	FMantleVehicleAsset Asset;
	FString Why;
	if (!FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Document.Get()), Document.Length()), Asset.Desc, Why))
	{
		return Refuse(OutError, FString::Printf(TEXT("the chassis's vehicle document is not valid: %s"), *Why));
	}

	// The skeleton: a root at the origin and a bone at each wheel centre, the root's children. A steered centreline wheel hangs under a
	// fork bone at its centre instead, so the fork can yaw it.
	FMantleSkinnedMesh& Body = Asset.Body;
	Body.Bones.Add({ FMantleVehicleChassisBuilder::RootBoneName(), INDEX_NONE });
	Body.ReferencePose.Add(FTransform::Identity);
	TArray<int32> WheelBones;
	for (int32 Index = 0; Index < Spec.WheelCount(); ++Index)
	{
		const FVector Centre = Spec.WheelCentre(Index);
		int32 Parent = 0;
		FTransform Pose(Centre);
		if (Spec.bCentreline && Spec.Axles[Spec.AxleOf(Index)].bSteer)
		{
			Parent = Body.Bones.Num();
			Body.Bones.Add({ Spec.ForkBoneOfAxle(Spec.AxleOf(Index)), 0 });
			Body.ReferencePose.Add(FTransform(Centre));
			Pose = FTransform::Identity;
		}
		WheelBones.Add(Body.Bones.Num());
		Body.Bones.Add({ Spec.WheelBone(Index), Parent });
		Body.ReferencePose.Add(Pose);
		Asset.WheelPositions.Add(Centre);
	}

	// A plane's control surfaces and engines: a bone each under the root, posed at the surface's hinge (its aerofoil), so a surface turns about its own
	// line and the rest of the body stays where it is, and at the engine's propeller hub or jet nozzle exit. None is a wheel, and the physics asset has no
	// body on any of them.
	int32 AileronBones[2] = { INDEX_NONE, INDEX_NONE };
	int32 ElevatorBone = INDEX_NONE;
	int32 RudderBone = INDEX_NONE;
	TArray<int32> EngineBones;
	if (Spec.Aircraft.bSet)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			AileronBones[Side] = Body.Bones.Num();
			Body.Bones.Add({ AileronBoneName(Side == 1), 0 });
			Body.ReferencePose.Add(FTransform(Spec.AileronBonePosition(Side == 1)));
		}
		ElevatorBone = Body.Bones.Num();
		Body.Bones.Add({ ElevatorBoneName(), 0 });
		Body.ReferencePose.Add(FTransform(Spec.ElevatorLocation()));
		RudderBone = Body.Bones.Num();
		Body.Bones.Add({ RudderBoneName(), 0 });
		Body.ReferencePose.Add(FTransform(Spec.RudderLocation()));
		for (int32 Engine = 0; Engine < Spec.Aircraft.EngineCount; ++Engine)
		{
			EngineBones.Add(Body.Bones.Num());
			Body.Bones.Add({ EngineBoneName(Spec.Aircraft, Engine), 0 });
			Body.ReferencePose.Add(FTransform(Spec.EngineLocation(Engine)));
		}
	}
	// A helicopter's two rotors: a bone each under the root at its hub, so each spins about its own hub and the rest of the body stays where it is.
	int32 MainRotorBone = INDEX_NONE;
	int32 TailRotorBone = INDEX_NONE;
	if (Spec.Helicopter.bSet)
	{
		MainRotorBone = Body.Bones.Num();
		Body.Bones.Add({ MainRotorBoneName(), 0 });
		Body.ReferencePose.Add(FTransform(Spec.MainRotorHub()));
		TailRotorBone = Body.Bones.Num();
		Body.Bones.Add({ TailRotorBoneName(), 0 });
		Body.ReferencePose.Add(FTransform(Spec.TailRotorHub()));
	}
	Asset.RootBoneTransform = FTransform::Identity;

	// The bones the movement blocks name are the body's, the posed ones free of the wheels (#821, the check Read makes of a published body).
	if (!FMantleVehicleAssetReader::ResolveMovementBones(Asset.Desc, Body.Bones, Why))
	{
		return Refuse(OutError, FString::Printf(TEXT("the chassis's vehicle document does not fit its body: %s"), *Why));
	}

	// The grey shapes: the body box on the root, a cylinder on each wheel bone.
	FMantleSkinnedMeshLOD& LOD = Body.LODs.AddDefaulted_GetRef();
	LOD.NumInfluences = 4;
	FShapeBuilder Shapes(LOD);
	const FMantleChassisBlock BodyBlock = Spec.BodyBlock();
	Shapes.AddBox(FVector3f(BodyBlock.Min), FVector3f(BodyBlock.Max), 0);
	// A helicopter's wheels are its skids' hidden contact points, so none is drawn: the skids below stand for them. A hovercraft's are where its pads lift,
	// under its skirt, and are not drawn either.
	const bool bDrawWheels = !Spec.Helicopter.bSet && !Spec.Hovercraft.bSet;
	for (int32 Index = 0; bDrawWheels && Index < Spec.WheelCount(); ++Index)
	{
		Shapes.AddWheel(FVector3f(Spec.WheelCentre(Index)), static_cast<float>(Spec.WheelRadius), static_cast<float>(Spec.WheelWidth), WheelBones[Index]);
	}
	// A hovercraft's: the skirt under the hull, the fan's pod behind it and the fin on its top, all on the root.
	if (Spec.Hovercraft.bSet)
	{
		for (const FMantleChassisBlock& Block : { Spec.SkirtBlock(), Spec.FanPodBlock(), Spec.HoverFinBlock() })
		{
			Shapes.AddBox(FVector3f(Block.Min), FVector3f(Block.Max), 0);
		}
	}
	// A helicopter's: the mast, the tail boom, the fin and the skids on the root, a cross of blades on each rotor's bone.
	if (Spec.Helicopter.bSet)
	{
		TArray<FMantleChassisBlock> Fixed = { Spec.MastBlock(), Spec.TailBoomBlock(), Spec.TailFinBlock(), Spec.SkidBlock(false), Spec.SkidBlock(true) };
		for (int32 Axle = 0; Axle < Spec.Axles.Num(); ++Axle)
		{
			Fixed.Add(Spec.SkidLegBlock(Axle, false));
			Fixed.Add(Spec.SkidLegBlock(Axle, true));
			Fixed.Add(Spec.SkidCrossTubeBlock(Axle));
		}
		for (const FMantleChassisBlock& Block : Fixed)
		{
			Shapes.AddBox(FVector3f(Block.Min), FVector3f(Block.Max), 0);
		}
		for (int32 Blade = 0; Blade < 2; ++Blade)
		{
			const FMantleChassisBlock Main = Spec.MainRotorBladeBlock(Blade);
			Shapes.AddBox(FVector3f(Main.Min), FVector3f(Main.Max), MainRotorBone);
			const FMantleChassisBlock Tail = Spec.TailRotorBladeBlock(Blade);
			Shapes.AddBox(FVector3f(Tail.Min), FVector3f(Tail.Max), TailRotorBone);
		}
	}
	// A plane's: a block per half-wing on its aileron's bone, the tailplane on the elevator's, the fin on the rudder's, and on each propeller's a cross of two
	// thin blades about X. A twin's propellers each get a nacelle on the root, from the propeller back to the wing's trailing edge. A jet has no blades: each
	// engine's nozzle is a box on its nozzle bone behind the fuselage's rear end, and an intake stands on the root each side of the fuselage.
	if (Spec.Aircraft.bSet)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FMantleChassisBlock Half = Spec.HalfWingBlock(Side == 1);
			Shapes.AddBox(FVector3f(Half.Min), FVector3f(Half.Max), AileronBones[Side]);
		}
		const FMantleChassisBlock Tail = Spec.TailplaneBlock();
		Shapes.AddBox(FVector3f(Tail.Min), FVector3f(Tail.Max), ElevatorBone);
		const FMantleChassisBlock Fin = Spec.FinBlock();
		Shapes.AddBox(FVector3f(Fin.Min), FVector3f(Fin.Max), RudderBone);
		if (Spec.Aircraft.bPropeller)
		{
			const double Radius = Spec.Aircraft.PropellerRadius;
			const double Thickness = PropellerHalfThickness * Spec.Scale;
			const double Width = PropellerHalfWidth * Spec.Scale;
			for (int32 Engine = 0; Engine < Spec.Aircraft.EngineCount; ++Engine)
			{
				const FVector Hub = Spec.PropellerHub(Engine);
				Shapes.AddBox(FVector3f(Hub + FVector(-Thickness, -Width, -Radius)), FVector3f(Hub + FVector(Thickness, Width, Radius)), EngineBones[Engine]);
				Shapes.AddBox(FVector3f(Hub + FVector(-Thickness, -Radius, -Width)), FVector3f(Hub + FVector(Thickness, Radius, Width)), EngineBones[Engine]);
				if (Spec.Aircraft.EngineCount > 1)
				{
					const double Half = NacelleHalfWidth * Spec.Scale;
					const double Back = Spec.Aircraft.Wing.X - 0.75 * Spec.Aircraft.Wing.Chord;
					Shapes.AddBox(FVector3f(FVector(Back, Hub.Y - Half, Hub.Z - Half)), FVector3f(FVector(Hub.X - Thickness - 1.0, Hub.Y + Half, Hub.Z + Half)), 0);
				}
			}
		}
		else
		{
			for (int32 Engine = 0; Engine < Spec.Aircraft.EngineCount; ++Engine)
			{
				const FMantleChassisBlock Nozzle = Spec.NozzleBlock(Engine);
				Shapes.AddBox(FVector3f(Nozzle.Min), FVector3f(Nozzle.Max), EngineBones[Engine]);
			}
			for (const bool bRight : { false, true })
			{
				const FMantleChassisBlock Intake = Spec.IntakeBlock(bRight);
				Shapes.AddBox(FVector3f(Intake.Min), FVector3f(Intake.Max), 0);
			}
		}
	}
	FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
	Section.MaterialIndex = 0;
	Section.FirstIndex = 0;
	Section.IndexCount = LOD.Indices.Num();
	Section.MaterialName = FName(SectionName);
	if (!Body.Validate(Why))
	{
		return Refuse(OutError, FString::Printf(TEXT("the chassis's body is not valid: %s"), *Why));
	}
	OutAsset = MoveTemp(Asset);
	return true;
}
