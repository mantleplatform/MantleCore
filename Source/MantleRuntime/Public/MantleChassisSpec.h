// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "MantleChassisDerive.h"
#include "MantleVehicleChassisBuilder.h"

class FJsonObject;

/**
 * What kind of vehicle a preset is (schema/vehicle-preset.schema.json `type`, #793): the family whose derive block it shares. A type selects
 * only that block, the stability target and the two camera constants. The cars are the sedan, sports car, limousine, pickup and van; the
 * trucks the truck and the six-wheeler; a motorcycle has one wheel on each axle, on the centreline, and balances itself; a plane flies on a
 * wing, a tailplane and a fin (chassis.aircraft) and stands on tricycle gear, its nose wheel on the centreline; a helicopter is lifted and steered
 * by its main rotor (chassis.helicopter) and stands on two skids, each a pair of hidden wheels that are always braked; a hovercraft floats on four
 * pads (chassis.hovercraft), suspensions with no wheel at the layout's hidden wheel points, and is pushed by a fan.
 */
enum class EMantleChassisType : uint8
{
	Car,
	Truck,
	Motorcycle,
	Plane,
	Helicopter,
	Hovercraft,
};

/** One axle of a chassis's layout. */
struct MANTLERUNTIME_API FMantleChassisAxle
{
	bool bSteer = false;
	bool bDrive = false;
	bool bHandbrake = false;
	/** One wheel, on the centreline, instead of a pair: a plane's nose wheel. (A layout whose every axle is like that is FMantleChassisSpec::bCentreline.) */
	bool bCentreline = false;
	/** How far back from the front axle, as a share of the wheelbase: 0 is the front axle, 1 the rearmost. */
	double Position = 0.0;
};

/** An axis-aligned block of a chassis, model space, cm. */
struct FMantleChassisBlock
{
	FVector Min = FVector::ZeroVector;
	FVector Max = FVector::ZeroVector;
};

/**
 * One lifting surface of a plane (chassis.aircraft.wing, tailplane and fin): its planform, where its aerofoil is, and the angles it flies on.
 * Every surface is one block centred on Y that turns as a whole on a bone at its aerofoil: an aileron turns a half-wing, the elevator the
 * tailplane, the rudder the fin. A wing and a tailplane lie flat, so Span is tip to tip along Y and Z their height; a fin stands on the
 * fuselage's top, so Span is its height and it has no Z of its own. Lengths are cm, areas m^2 and angles degrees, which do not scale.
 */
struct MANTLERUNTIME_API FMantleChassisSurface
{
	double Span = 0.0;
	double Chord = 0.0;
	/** Where the aerofoil's quarter-chord line is along X: the lift acts here and the bone sits here, so the block runs 0.25 Chord ahead of it to 0.75 Chord behind. */
	double X = 0.0;
	/** The middle of a wing's or tailplane's block, up; unused by the fin. */
	double Z = 0.0;
	/** The aerofoil's area, m^2 (the wing's: both halves together). The preset's own; a resized chassis's is in its handling (FMantleChassisHandling). */
	double AreaM2 = 0.0;
	/** FMantleVehicleAerofoil's Camber, MaxControlAngle and StallAngle. */
	double Camber = 0.0;
	double MaxControlAngle = 0.0;
	double StallAngle = 20.0;

	/** Span x chord, cm^2: what the area follows when the surface is resized. */
	double Planform() const { return Span * Chord; }
};

/**
 * What a plane preset adds to the chassis (schema/vehicle-preset.schema.json chassis.aircraft): the fuselage is the chassis's body box, and
 * this is the rest of the airframe, its engines and its cockpit. Model space, cm. The engine's height is the fuselage's middle.
 */
struct MANTLERUNTIME_API FMantleChassisAircraft
{
	/** The preset has an aircraft block. */
	bool bSet = false;
	FMantleChassisSurface Wing;
	FMantleChassisSurface Tailplane;
	FMantleChassisSurface Fin;
	/** Propellers: one engine on the centreline at the nose, or two, one each side of it on the wing. Jets: one nozzle on the centreline behind the tail, or two side by side. */
	int32 EngineCount = 1;
	/** Each engine's thrust at full throttle, N. The preset's own; a resized chassis's is in its handling. */
	double EngineThrustNewtons = 0.0;
	/** Where the engines push from along X: the propeller hubs, or the jets' nozzle exits. */
	double EngineX = 0.0;
	/** The airframe has a propeller block: its engines turn propellers. Without one they are jets, each pushing from a nozzle behind the fuselage's tail, and nothing spins. */
	bool bPropeller = false;
	double PropellerRadius = 0.0;
	/** A propeller's spin, rpm: cosmetic. With two engines the right one turns the other way. */
	double PropellerRpm = 0.0;
	/** The most the airframe is allowed to fly, cm/s (FMantleVehicleDefinition AirSpeedLimitCmPerSec). */
	double AirSpeedLimitCmPerSec = 0.0;
	/** The speed the airframe is trimmed to fly level at, elevator centred, cm/s (chassis.aircraft.cruiseSpeed). Not in a vehicle document: it is what
	 *  FMantleChassisDerive::LevelFlight solves the tailplane's camber and CruiseThrottle at, so a resized airframe flies level hands-off as the preset does. */
	double CruiseSpeedCmPerSec = 0.0;
	/** The throttle (0 to 1 of the engines' thrust) that flies the airframe level at its cruise speed: what the pilot's throttle returns to when W and S
	 *  are let go in the air (FMantleVehicleDefinition CruiseThrottle). A ratio, so a resized chassis keeps it. */
	double CruiseThrottle = 0.0;
	/** Where the pilot's left hand holds the throttle and the right the stick. */
	FVector ThrottleGrip = FVector::ZeroVector;
	FVector StickGrip = FVector::ZeroVector;
};

/**
 * What a helicopter preset adds to the chassis (schema/vehicle-preset.schema.json chassis.helicopter): the cabin is the chassis's body box, and this is
 * the rest, its rotors, tail and cockpit. Model space, cm. The main rotor's hub is over the centre of mass, so its lift turns nothing; the skids are the
 * layout's wheels, which stay hidden.
 */
struct MANTLERUNTIME_API FMantleChassisHelicopter
{
	/** The preset has a helicopter block. */
	bool bSet = false;
	/** The main rotor: tip to tip, the hub's height, its lift at full collective (N; the preset's own, a resized chassis's is in its handling) and its
	 *  cosmetic spin (rpm). */
	double RotorDiameter = 0.0;
	double RotorZ = 0.0;
	double RotorThrustNewtons = 0.0;
	double RotorRpm = 0.0;
	/** The tail boom, from the cabin's rear end back by its length, centred on Y, its middle at BoomZ. */
	double BoomLength = 0.0;
	double BoomWidth = 0.0;
	double BoomHeight = 0.0;
	double BoomZ = 0.0;
	/** The tail rotor, beside the boom's left side at the fin: cosmetic, since the main rotor's module turns the helicopter on Yaw. */
	double TailRotorDiameter = 0.0;
	double TailRotorRpm = 0.0;
	/** The fin standing on the boom's end. */
	double FinHeight = 0.0;
	double FinChord = 0.0;
	/** The collective (0 to 1 of the rotor's thrust) that holds the weight: the weight over the thrust (FMantleVehicleDefinition CruiseThrottle). A ratio, so a
	 *  resized chassis, whose thrust follows its mass, keeps it. */
	double HoverCollective = 0.0;
	/** The most the helicopter is allowed to fly, cm/s (FMantleVehicleDefinition AirSpeedLimitCmPerSec). */
	double AirSpeedLimitCmPerSec = 0.0;
	/** Where the pilot's left hand holds the collective and the right the cyclic. */
	FVector CollectiveGrip = FVector::ZeroVector;
	FVector CyclicGrip = FVector::ZeroVector;
};

/**
 * What a hovercraft preset adds to the chassis (schema/vehicle-preset.schema.json chassis.hovercraft): the hull is the chassis's body box, whose ground
 * clearance is the hover height, and this is its lift, push, fin, skirt and damping. Model space, cm. The pads stand at the layout's wheel points on the
 * hull's underside; the layout's wheels are never drawn and drive nothing.
 */
struct MANTLERUNTIME_API FMantleChassisHovercraft
{
	/** The preset has a hovercraft block. */
	bool bSet = false;
	/** Each pad: its push starts PadTravel below it and its trace PadRaise above it; its spring rate (the preset's own, a resized chassis's is in its
	 *  handling) and damping are the suspension's. */
	double PadTravel = 0.0;
	double PadRaise = 0.0;
	double PadSpringRate = 0.0;
	double PadDamping = 0.0;
	/** The fan: its push at rest, N (the preset's own; a resized chassis's is in its handling), the share of it the reverse vanes give backwards, and how
	 *  far the vanes turn it at full steering, degrees. */
	double FanThrustNewtons = 0.0;
	double ReverseShare = 0.0;
	double VectorAngle = 0.0;
	/** The fan's pod: its middle at (FanX, 0, FanZ), where the fan pushes from, and its size. */
	double FanX = 0.0;
	double FanZ = 0.0;
	double FanLength = 0.0;
	double FanWidth = 0.0;
	double FanHeight = 0.0;
	/** The fin on the hull's top, FinChord long about FinX, and its rudder: area (m^2; the preset's own, a resized chassis's is in its handling) and angles. */
	double FinX = 0.0;
	double FinHeight = 0.0;
	double FinChord = 0.0;
	double FinAreaM2 = 0.0;
	double FinMaxControlAngle = 0.0;
	double FinStallAngle = 20.0;
	/** The drawn skirt: how much wider than the hull it is each way, and how far its underside is off the ground. */
	double SkirtOverhang = 0.0;
	double SkirtGap = 0.0;
	/** What stands in for a tyre's grip while a pad touches the ground: per second against the sideways speed, the yaw rate and (the skirt brake) the
	 *  velocity along the hull, each at most its cap (cm/s^2, degrees/s^2, cm/s^2). Accelerations, so they do not scale. */
	double LateralPerSecond = 0.0;
	double MaxLateralAccel = 0.0;
	double YawPerSecond = 0.0;
	double MaxYawAccelDegrees = 0.0;
	double SkirtBrakePerSecond = 0.0;
	double MaxSkirtBrakeAccel = 0.0;

	/** How far a pad comes down at rest, cm: its travel less the hover height. What its spring holds the weight over. */
	double Compression(const double HoverHeight) const { return PadTravel - HoverHeight; }
};

/**
 * A preset's own size (#793): the dimensions its hand-tuned handling belongs to. FMantleChassisDerive multiplies that handling by how a chassis's
 * dimensions differ from these, so a chassis with these dimensions has exactly the preset's handling and a dimension edited on its own moves only
 * the numbers that depend on it. Set when the preset is read; Scaled and any edit of the chassis leave it alone. WheelWidth is kept with the rest
 * but no rule reads it: how wide a tyre is does not change how a vehicle drives (#793, customisation never changes physics).
 */
struct MANTLERUNTIME_API FMantleChassisSize
{
	double Wheelbase = 0.0;
	double Track = 0.0;
	double WheelRadius = 0.0;
	double WheelWidth = 0.0;
	double Length = 0.0;
	double Width = 0.0;
	double Height = 0.0;
	double Clearance = 0.0;
	/** The archetype's axles, front first: how many wheels it has. */
	TArray<FMantleChassisAxle> Axles;
	bool bCentreline = false;
	/** The archetype's airframe: a plane's wing, tailplane and fin areas follow how much their planforms differ from these. */
	FMantleChassisAircraft Aircraft;
	/** The archetype's rotors and tail. */
	FMantleChassisHelicopter Helicopter;
	/** The archetype's pads, fan and fin: a hovercraft's spring rate follows how much its pads come down against these, and its rudder its fin's planform. */
	FMantleChassisHovercraft Hovercraft;

	/** A pair of wheels to an axle, or one for an axle on the centreline (the whole layout's, or the axle's own). */
	int32 WheelCount() const
	{
		int32 Count = 0;
		for (const FMantleChassisAxle& Axle : Axles)
		{
			Count += (bCentreline || Axle.bCentreline) ? 1 : 2;
		}
		return Count;
	}
};

/**
 * A chassis, read (#793): the size, layout, seats and handling a vehicle's body, wheels and document are made from. Parse reads a preset's
 * text; Scaled sizes one by a uniform factor and derives its handling from the new dimensions (FMantleChassisDerive); ComposeDocument writes
 * its vehicle document and FMantleVehicleChassisBuilder::MakeAsset builds its grey body, skeleton and Desc.
 *
 * Model space, cm: +X forward, +Y right, +Z up, the origin on the ground under the middle of the wheelbase, the driver on -Y.
 * Value semantics: a copy shares nothing it can change. No UObjects: any thread.
 */
struct MANTLERUNTIME_API FMantleChassisSpec
{
	/** How the steering wheel is pitched, degrees: a typical wheel (schema/vehicle.schema.json steeringWheel.rotation). */
	static constexpr double SteeringWheelPitch = -20.0;

	/** The fewest and most axles a layout has. */
	static constexpr int32 MinAxles = 2;
	static constexpr int32 MaxAxles = 4;

	/** The smallest and largest uniform scale a selector (a preset id and a scale) names. Bounds on what a demo may ask a machine to build, not the size envelope (#793). */
	static constexpr double MinScale = 0.25;
	static constexpr double MaxScale = 2.5;

	/** The longest selector, characters. */
	static constexpr int32 MaxSelectorLength = 48;

	/** How thick a plane's wing, tailplane and fin blocks are, as a share of their chord: an eighth, which is exact in binary, so every coordinate of the airframe is. */
	static constexpr double SurfaceThickness = 0.125;

	/** Where a half-wing's aerofoil sits across its span, as a share of the half-span from the fuselage: out where an aileron's lift has most roll to give. */
	static constexpr double WingAerofoilSpanShare = 0.7;

	/**
	 * How wide a jet's square nozzle is, as a share of the fuselage's narrower side: a single engine's, and each of a twin's, whose two sit side by side
	 * TwinNozzleSpread of the fuselage's width either side of the centreline (schema/validate.py PLANE_NOZZLE_SIZE and PLANE_TWIN_NOZZLE_SPREAD).
	 */
	static constexpr double SingleNozzleSize = 0.7;
	static constexpr double TwinNozzleSize = 0.4;
	static constexpr double TwinNozzleSpread = 0.25;

	/** A jet's intake each side of the fuselage: how far it stands out and how tall it is, as shares of the fuselage's width and height, and how far ahead of the wing it reaches, as a share of the fuselage's length. */
	static constexpr double IntakeDepthShare = 0.25;
	static constexpr double IntakeHeightShare = 0.5;
	static constexpr double IntakeLengthShare = 0.2;

	/** How far the tail rotor's hub stands out from the boom's left side, cm at scale 1 (schema/validate.py HELI_TAIL_ROTOR_GAP). */
	static constexpr double TailRotorGap = 10.0;

	/** A helicopter's grey parts, cm at scale 1: the mast's half-width, and each main and tail rotor blade's half-chord and half-thickness. */
	static constexpr double MastHalfWidth = 12.0;
	static constexpr double RotorBladeHalfChord = 15.0;
	static constexpr double RotorBladeHalfThickness = 3.0;
	static constexpr double TailBladeHalfChord = 6.0;
	static constexpr double TailBladeHalfThickness = 2.0;

	/**
	 * A helicopter's skid tubes, as shares of the (hidden) wheel radius: the tube's square side, how far its underside is off the ground at rest (the
	 * wheels' sag takes most of that), and how far it runs past the front axle and behind the rear one.
	 */
	static constexpr double SkidTubeShare = 0.4;
	static constexpr double SkidTubeLiftShare = 0.25;
	static constexpr double SkidFrontReachShare = 2.0;
	static constexpr double SkidRearReachShare = 1.0;

	/** The preset's id. */
	FString Id;
	EMantleChassisType Type = EMantleChassisType::Car;

	/** How much larger than the preset's own size this is: 1 for the preset, the product of every Scaled factor. */
	double Scale = 1.0;
	/** The preset's own dimensions: what the derived handling is a ratio against. Not the dimensions below, which a creator may edit. */
	FMantleChassisSize ArchetypeSize;

	/** The preset's `defaults`, read-only: a document fragment ComposeDocument completes in a copy. */
	TSharedPtr<const FJsonObject> Defaults;
	/** The preset's hand-tuned handling: `defaults` and the chassis's centre of mass, read. FMantleChassisDerive returns exactly this at ArchetypeSize. */
	FMantleChassisHandling Archetype;
	/** The preset's derive block: what a ratio of dimensions cannot say. */
	FMantleChassisDeriveConstants Constants;
	/** When set, the handling the document is composed with, over `defaults` (a Scaled chassis); unset, the document is the preset's own. */
	TOptional<FMantleChassisHandling> Derived;

	/** The chassis's dimensions. A preset's are its ArchetypeSize; Scaled resizes them; an editor may change any one of them and Derive follows. */
	double Wheelbase = 0.0;
	double Track = 0.0;
	double WheelRadius = 0.0;
	double WheelWidth = 0.0;
	/** The axles, front first. */
	TArray<FMantleChassisAxle> Axles;
	double Length = 0.0;
	double Width = 0.0;
	double Height = 0.0;
	double Clearance = 0.0;
	FVector CenterOfMass = FVector::ZeroVector;
	FVector Seat = FVector::ZeroVector;
	FVector Hub = FVector::ZeroVector;
	double RimRadius = 0.0;
	/** seats.passenger is set: the car has a passenger seat, the driver's mirrored across Y. */
	bool bPassenger = false;
	/** One wheel per axle, on the centreline (layout.centreline): a motorcycle. A plane has only its nose axle so (FMantleChassisAxle::bCentreline). */
	bool bCentreline = false;
	/** The seat has a steering wheel (a rider's has handlebars instead, and a pilot's a stick and a throttle). */
	bool bSteeringWheel = true;
	FString DriverSeat = TEXT("car");
	FName ForkBone;
	FMantleVehicleChassisRider Rider;
	/** Where the body box's middle is along X, cm: 0, the middle of the wheelbase, for everything but a plane or a helicopter, whose fuselage or cabin is not
	 *  centred on its landing gear or skids. */
	double BodyX = 0.0;
	/** A plane's airframe and cockpit (chassis.aircraft); bSet is false for everything else. */
	FMantleChassisAircraft Aircraft;
	/** A helicopter's rotors, tail and cockpit (chassis.helicopter); bSet is false for everything else. */
	FMantleChassisHelicopter Helicopter;
	/** A hovercraft's pads, fan, fin, skirt and damping (chassis.hovercraft); bSet is false for everything else. */
	FMantleChassisHovercraft Hovercraft;

	/** Whether axle Axle has one wheel, on the centreline: the whole layout's, or the axle's own. */
	bool AxleIsCentreline(const int32 Axle) const { return bCentreline || Axles[Axle].bCentreline; }

	/** A pair of wheels to an axle, or one for an axle on the centreline. */
	int32 WheelCount() const
	{
		int32 Count = 0;
		for (int32 Axle = 0; Axle < Axles.Num(); ++Axle)
		{
			Count += AxleIsCentreline(Axle) ? 1 : 2;
		}
		return Count;
	}

	/** The first wheel of axle Axle, counted from the front axle's left (a pair is made left then right; a centreline axle has the one). */
	int32 FirstWheelOf(const int32 Axle) const
	{
		int32 First = 0;
		for (int32 Before = 0; Before < Axle; ++Before)
		{
			First += AxleIsCentreline(Before) ? 1 : 2;
		}
		return First;
	}

	/** The axle of wheel Index, counted from the front. */
	int32 AxleOf(const int32 Index) const
	{
		for (int32 Axle = 0; Axle < Axles.Num(); ++Axle)
		{
			if (Index < FirstWheelOf(Axle) + (AxleIsCentreline(Axle) ? 1 : 2))
			{
				return Axle;
			}
		}
		return Axles.Num() - 1;
	}

	/** The bone of wheel Index. */
	FName WheelBone(int32 Index) const;

	/** The fork bone a steered centreline wheel on axle Axle hangs under. */
	FName ForkBoneOfAxle(int32 Axle) const;

	/** The centre of wheel Index: its axle's X, the side (0 is left, -Y; a pair is left then right) across the track; a centreline wheel is on Y 0. */
	FVector WheelCentre(const int32 Index) const
	{
		const int32 Axle = AxleOf(Index);
		const double Y = AxleIsCentreline(Axle) ? 0.0 : (Index == FirstWheelOf(Axle) ? -0.5 : 0.5) * Track;
		return FVector(Wheelbase * (0.5 - Axles[Axle].Position), Y, WheelRadius);
	}

	/** The top of the body box, cm above the ground: where a plane's fin stands. */
	double BodyTop() const { return Clearance + Height; }

	/** The body box (the fuselage of a plane). */
	FMantleChassisBlock BodyBlock() const;

	/**
	 * A plane's lifting surfaces as the blocks the grey body draws and the document collides (Aircraft.bSet), in the same frame: the whole wing, one
	 * half of it (the left, -Y, or the right) with the aileron bone that turns it, the tailplane and the fin. A block is SurfaceThickness of its chord
	 * thick, runs 0.25 chord ahead of its aerofoil's quarter-chord line to 0.75 behind, and a fin stands from BodyTop() up by its height.
	 */
	FMantleChassisBlock WingBlock() const;
	FMantleChassisBlock HalfWingBlock(bool bRight) const;
	FMantleChassisBlock TailplaneBlock() const;
	FMantleChassisBlock FinBlock() const;

	/** Where a plane's aerofoils act, cm: the wing's two halves (WingAerofoilSpanShare of the half-span out), the elevator, and the rudder (the fin's middle). */
	FVector WingAerofoilLocation(bool bRight) const;
	FVector ElevatorLocation() const;
	FVector RudderLocation() const;

	/** Where an aileron's bone hinges: the middle of its half-wing's span, on the wing's quarter-chord line. */
	FVector AileronBonePosition(bool bRight) const;

	/** The hub of engine Engine's propeller (0 with one engine: the centreline; with two, 0 is the left): a third of the half-span out for two, at the fuselage's middle height. */
	FVector PropellerHub(int32 Engine) const;

	/**
	 * The middle of jet engine Engine's nozzle exit (0 with one engine: the centreline; with two, 0 is the left), at EngineX behind the tail and the
	 * fuselage's middle height; a twin's two are TwinNozzleSpread of the fuselage's width either side of the centreline.
	 */
	FVector NozzleLocation(int32 Engine) const;

	/** Where engine Engine pushes from: its propeller's hub (Aircraft.bPropeller), or its jet's nozzle exit. */
	FVector EngineLocation(int32 Engine) const;

	/** A jet engine's nozzle as the grey body draws it (no collision): a square box from its exit back at EngineX forward to the fuselage's rear end. */
	FMantleChassisBlock NozzleBlock(int32 Engine) const;

	/** A jet's intake on the left (-Y) or right side of the fuselage (no collision): a box against the fuselage's side, from the wing's leading edge forward. */
	FMantleChassisBlock IntakeBlock(bool bRight) const;

	/** A helicopter's main rotor hub: over the centre of mass, at the rotor's height, so the rotor's lift passes through the centre of mass when it is level. */
	FVector MainRotorHub() const;

	/** A helicopter's tail rotor hub: half the fin's chord ahead of the boom's end, TailRotorGap out from the boom's left side, at the boom's middle height. */
	FVector TailRotorHub() const;

	/**
	 * A helicopter's blocks, the same frame. The boom runs from the cabin's rear end back by its length; the fin stands on its end, SurfaceThickness of its chord
	 * thick; these two and the cabin are what the document collides. The mast stands from the cabin's top to the main rotor's blades. The main rotor's blades
	 * are a cross about its hub (Blade 0 along X, 1 along Y), the tail rotor's a cross in the XZ plane about its hub (0 along X, 1 along Z). Each skid is a
	 * tube along one side's wheels, standing on a leg at each axle, and a cross-tube under the cabin joins the legs of each axle. None but the cabin, boom
	 * and fin collides.
	 */
	FMantleChassisBlock TailBoomBlock() const;
	FMantleChassisBlock TailFinBlock() const;
	FMantleChassisBlock MastBlock() const;
	FMantleChassisBlock MainRotorBladeBlock(int32 Blade) const;
	FMantleChassisBlock TailRotorBladeBlock(int32 Blade) const;
	FMantleChassisBlock SkidBlock(bool bRight) const;
	FMantleChassisBlock SkidLegBlock(int32 Axle, bool bRight) const;
	FMantleChassisBlock SkidCrossTubeBlock(int32 Axle) const;

	/** A hovercraft's pad Index (the layout's wheel Index): at the wheel's X and Y, on the hull's underside (the hover height). */
	FVector PadLocation(int32 Index) const;

	/** Where a hovercraft's fan pushes from: its pod's middle. */
	FVector FanLocation() const;

	/** Where a hovercraft's rudder acts: the fin's middle. */
	FVector HoverRudderLocation() const;

	/**
	 * A hovercraft's drawn blocks, the same frame, none of them collided (only the hull is): the skirt under the hull, SkirtOverhang wider each way, from
	 * SkirtGap above the ground up to the hull; the fan's pod; the fin on the hull's top, SurfaceThickness of its chord thick.
	 */
	FMantleChassisBlock SkirtBlock() const;
	FMantleChassisBlock FanPodBlock() const;
	FMantleChassisBlock HoverFinBlock() const;

	/** A type's name as a preset spells it ("car", "truck", "motorcycle", "plane", "helicopter", "hovercraft"), and the type a name spells. */
	static const TCHAR* TypeName(EMantleChassisType Kind);
	static bool ParseType(const FString& Name, EMantleChassisType& OutType);

	/** Whether Text is a preset id: the name of a preset file, a lowercase letter and then lowercase letters, digits and dashes, so it names nothing outside the presets' directory. */
	static bool IsPresetId(const FString& Text);

	/** Reads a preset's text (schema/vehicle-preset.schema.json) into OutSpec. False, with the reason, when it is not a whole preset. */
	static bool Parse(const FString& PresetJsonText, FMantleChassisSpec& OutSpec, FString& OutError);

	/**
	 * The vehicle document of Spec (schema/vehicle.schema.json, every default filled) as UTF-8 text: its defaults, with Derived's numbers over
	 * them when it has any, completed with schema defaults, plus the chassis's own wheels, collision box, centre of mass and seats. Spec is
	 * not changed. OutHeld, when given, gets a line for every number the document's ranges held ("path value held to limit",
	 * FMantleVehicleChassisDocument::Derive's).
	 */
	static FString ComposeDocument(const FMantleChassisSpec& Spec, TArray<FString>* OutHeld = nullptr);

	/**
	 * Spec sized Factor times as large (#793): the layout, body, seat, steering wheel, rider and (for a plane, a helicopter or a hovercraft) airframe, rotors,
	 * cockpit, pads, fan, fin and skirt geometry scaled by Factor, then the handling
	 * re-derived from the new dimensions (FMantleChassisDerive, every feel slider at 0), its centre of mass the derived one. A Factor of 1
	 * returns Spec itself, its hand-tuned handling untouched: a preset keeps its own numbers and only a resized chassis uses derived ones. A
	 * A Factor that is not a finite number above 0 returns Spec too. The steering wheel's rim radius is held to the document's 10 to 30 cm.
	 * A plane's tailplane camber and cruise throttle are then solved again (Trim): its weight, wings and thrust no longer follow the preset's trim.
	 */
	static FMantleChassisSpec Scaled(const FMantleChassisSpec& Spec, double Factor);

	/**
	 * Trims a plane (Aircraft.bSet with a cruise speed): sets the tailplane's camber and Aircraft.CruiseThrottle to what flies it level at its cruise speed
	 * with the elevator centred, from its current dimensions and handling (FMantleChassisDerive::LevelFlight; the same arithmetic as schema/validate.py's
	 * trim). Scaled does it; call it after changing a plane's dimensions in any other way. Nothing changes for anything that is not a plane. When no camber
	 * within the elevator's reach holds level flight at that speed (the plane is too heavy for its wings) the spec keeps its numbers, a line saying so is
	 * added to the Clamped of its derived handling, and this returns false.
	 */
	static bool Trim(FMantleChassisSpec& InOutSpec);

	/**
	 * Splits a selector into a preset id and a scale. A selector is a preset id, or an id then '@' then a scale: "sedan", "sedan@0.7". The
	 * scale is plain decimal digits with at most three after the point, MinScale to MaxScale; a selector names exactly one chassis on every
	 * machine, whatever its locale, and names nothing but a preset the plugin carries. False, with the reason, for anything else.
	 */
	static bool ParseSelector(const FString& Selector, FString& OutPresetId, double& OutScale, FString& OutError);

	/** The selector of preset PresetId at the scale Factor, the shortest spelling ParseSelector reads back to the same scale: "sedan" at 1, "sedan@0.7" at 0.7. */
	static FString MakeSelector(const FString& PresetId, double Factor);
};
