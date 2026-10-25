// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleChassisSpec.h"

/** The sizes of a vehicle a creator shapes in the Build a vehicle window (#793 step 4), each in centimetres. */
enum class EMantleVehicleDimension : uint8
{
	Wheelbase,
	Track,
	WheelRadius,
	WheelWidth,
	BodyLength,
	BodyWidth,
	BodyHeight,
	GroundClearance,
};

/** All of the dimensions, in the order the window lists them. */
inline constexpr EMantleVehicleDimension MantleVehicleDimensions[] = {
	EMantleVehicleDimension::Wheelbase,
	EMantleVehicleDimension::Track,
	EMantleVehicleDimension::WheelRadius,
	EMantleVehicleDimension::WheelWidth,
	EMantleVehicleDimension::BodyLength,
	EMantleVehicleDimension::BodyWidth,
	EMantleVehicleDimension::BodyHeight,
	EMantleVehicleDimension::GroundClearance,
};

/** The eight sizes a creator shapes, centimetres. Nothing else about a vehicle is chosen; Mantle derives the rest (FMantleChassisDerive). */
struct MANTLEEDITOR_API FMantleVehicleDimensions
{
	double Wheelbase = 0.0;
	double Track = 0.0;
	double WheelRadius = 0.0;
	double WheelWidth = 0.0;
	double BodyLength = 0.0;
	double BodyWidth = 0.0;
	double BodyHeight = 0.0;
	double GroundClearance = 0.0;

	double Get(EMantleVehicleDimension Dimension) const;
	void Set(EMantleVehicleDimension Dimension, double Value);

	/** True when every size is within Tolerance centimetres of Other's. */
	bool Equals(const FMantleVehicleDimensions& Other, double Tolerance = 1.0e-9) const;

	/** The sizes as text, to the thousandth of a centimetre: what a vehicle built from them is told apart by. */
	FString Signature() const;
};

/** A closed range of centimetres. */
struct FMantleDimensionRange
{
	double Min = 0.0;
	double Max = 0.0;

	double Clamp(double Value) const { return FMath::Clamp(Value, Min, Max); }
};

/**
 * How a vehicle type is shaped and described in the window (#821 M5). A ground vehicle (a car, a truck, a motorcycle) is shaped by its eight
 * sizes. The others are shaped by one Size, which grows the whole vehicle together through FMantleChassisSpec::Scaled, because a wing, a rotor or a
 * fan is sized for the weight it lifts and moving one of its parts on its own would leave the vehicle out of trim.
 */
enum class EMantleVehicleFamily : uint8
{
	/** A car, truck or motorcycle: eight sizes; weight, turning circle and top speed. */
	Ground,
	/** A plane or a jet: Size; weight, wing loading, take-off speed and top speed (the air-speed limit). */
	Aircraft,
	/** A helicopter: Size; weight and the throttle that hovers it. */
	Helicopter,
	/** A hovercraft: Size and a top-speed feel; weight, hover height and top speed. */
	Hovercraft,
};

/** The four feel sliders of FMantleChassisFeel (#793): how much grippier, stronger, stiffer or faster than the type's own a vehicle is, each up to 30 percent either way. */
enum class EMantleVehicleFeel : uint8
{
	Grip,
	Power,
	Stiffness,
	TopSpeed,
};

/** All of the feels, in the order the window lists them. */
inline constexpr EMantleVehicleFeel MantleVehicleFeels[] = {
	EMantleVehicleFeel::Grip,
	EMantleVehicleFeel::Power,
	EMantleVehicleFeel::Stiffness,
	EMantleVehicleFeel::TopSpeed,
};

/** What a shaped vehicle will be like, in the numbers the window says in words. Which of them are said depends on Family. */
struct FMantleVehicleSummary
{
	EMantleVehicleFamily Family = EMantleVehicleFamily::Ground;
	double MassKg = 0.0;
	/** The width of the circle it can turn in, cm (a ground vehicle's). */
	double TurningCircleCm = 0.0;
	/** A ground vehicle's top speed (its speed limit), a plane's (its air-speed limit) or a hovercraft's. */
	double TopSpeedKmh = 0.0;
	/** A plane's wing loading, kg per square metre of wing. */
	double WingLoadingKgM2 = 0.0;
	/** The speed a plane leaves the ground at, km/h. */
	double TakeOffKmh = 0.0;
	/** The share (0 to 1) of a helicopter's rotor lift that holds its weight: the throttle that hovers it. */
	double HoverThrottle = 0.0;
	/** How high a hovercraft floats, cm. */
	double HoverHeightCm = 0.0;
};

/**
 * The shape step of the Build a vehicle window, apart from Slate so tests can run it (#793 step 4). A creator starts from a preset (the
 * archetype: FMantleChassisSpec as it is at its own size, hand-tuned handling) and moves eight sizes. Every change goes through here:
 *   - Edit holds a size to its range and says so in plain words (a range is the archetype's size from 0.25 to 2.5 times, held inside the vehicle
 *     document's limits, and apart enough that wheels never touch);
 *   - Reshape makes the spec for those sizes: the sizes themselves, the seat, the steering wheel and the rider moved with the body the way a
 *     real vehicle keeps its driver, the handling derived from the new sizes (FMantleChassisDerive, public API only) and the centre of mass
 *     the derived one, as FMantleChassisSpec::Scaled does for a uniform resize. The archetype's own sizes give the archetype back, hand-tuned;
 *   - Summarise and Warnings say what the result is like.
 * Pure: the same inputs give the same spec on every machine. No UObjects.
 */
struct MANTLEEDITOR_API FMantleVehicleShape
{
	/** The longest wheelbase a vehicle may have, cm (the size envelope of #793 section 4, provisional until it is measured). */
	static constexpr double MaxWheelbase = 1200.0;

	/** Axles stay at least this many wheel radii apart (a wheel is 2 radii across), so neighbouring wheels never touch. */
	static constexpr double MinAxleGapPerRadius = 2.2;

	/** The track stays at least this many wheel widths, so the left and right wheels never touch. */
	static constexpr double MinTrackPerWidth = 1.25;

	/** A wheel may stick out of the body's box by this much before the window says so, cm. */
	static constexpr double WheelsOutsideBody = 10.0;

	/**
	 * The pitch a plane lifts off at, degrees: the nose-up angle its tail-strike limit allows (schema/validate.py PLANE_TAIL_STRIKE_DEGREES), so the
	 * lift at take-off is the wing's at this angle of attack plus its camber.
	 */
	static constexpr double TakeOffPitchDegrees = 8.0;

	/** The air's density, kg per cubic metre, and gravity, m per second squared: the engine's own (schema/validate.py PLANE_AIR_DENSITY and PLANE_GRAVITY). */
	static constexpr double AirDensity = 1.225;
	static constexpr double Gravity = 9.8;

	/** The rigid body's linear damping of a hovercraft, per second (FMantleVehicleChassis LinearDamping, schema/validate.py HOVER_LINEAR_DAMPING): it slows the hull with its drag. */
	static constexpr double HoverLinearDamping = 0.01;

	/** How Spec is shaped and described. */
	static EMantleVehicleFamily FamilyOf(const FMantleChassisSpec& Spec);

	/** True for the types shaped by one Size (everything but the ground vehicles). */
	static bool UsesSize(const FMantleChassisSpec& Spec) { return FamilyOf(Spec) != EMantleVehicleFamily::Ground; }

	/** True for a type whose wheels are never drawn (a helicopter's skids and a hovercraft's pads): it has no wheel size to shape. */
	static bool WheelsHidden(const FMantleChassisSpec& Spec);

	/**
	 * The speed a plane of this mass and wing leaves the ground at, km/h: the weight held by the wing's lift at TakeOffPitchDegrees of angle of attack.
	 * The engine's aerofoil lifts 2 sin((angle of attack + camber) x 90 / stall angle) per square metre of dynamic pressure, none past the stall
	 * angle's double. 0 for anything that is not a plane.
	 */
	static double TakeOffSpeedKmh(const FMantleChassisSpec& Spec);

	/**
	 * The speed a hovercraft tops out at on the flat, km/h: where the fan's push, falling off to nothing at the handling's speed limit, meets the hull's
	 * drag and the rigid body's damping (schema/validate.py _hovercraft_top_speed). 0 for anything that is not a hovercraft.
	 */
	static double HoverTopSpeedKmh(const FMantleChassisSpec& Spec);

	/** The sizes of Spec. */
	static FMantleVehicleDimensions DimensionsOf(const FMantleChassisSpec& Spec);

	/** The sizes of Spec's archetype (Spec.ArchetypeSize): the preset's own, whatever Spec has been changed to. A spec with none has its own sizes. */
	static FMantleVehicleDimensions ArchetypeDimensionsOf(const FMantleChassisSpec& Spec);

	/** False for the track of a vehicle whose wheels sit on its centreline (a motorcycle): it has none to change; and for the wheel size of one whose wheels are hidden. */
	static bool IsEditable(const FMantleChassisSpec& Archetype, EMantleVehicleDimension Dimension);

	/** The range a size's slider covers whatever the other sizes are: 0.25 to 2.5 times the archetype's, inside the document's limits. */
	static FMantleDimensionRange SliderRange(const FMantleChassisSpec& Archetype, EMantleVehicleDimension Dimension);

	/** The range a size may take with the others as they are: its slider range, narrowed so the wheels do not touch each other. */
	static FMantleDimensionRange HeldRange(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions, EMantleVehicleDimension Dimension);

	/**
	 * Dimensions with one size set to Wanted, held to HeldRange. Only that size changes. When it had to be held, OutNotes gets one plain
	 * sentence saying so. A size that is not editable, or a Wanted that is not a number, changes nothing.
	 */
	static FMantleVehicleDimensions Edit(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions, EMantleVehicleDimension Dimension,
		double Wanted, TArray<FText>& OutNotes);

	/** True when the sizes differ from the archetype's by more than a hundredth of a centimetre. */
	static bool IsShaped(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions);

	/**
	 * The spec for these sizes (held already: Edit keeps them so). Unshaped sizes give the archetype itself, its hand-tuned handling untouched.
	 * Otherwise a copy with the sizes set, the seat moved the way the body moved (along with the wheelbase, across with the body's width, up
	 * with the ground clearance), the steering wheel and a rider's grips and pegs carried with the seat, the handling derived and the centre of
	 * mass the derived one.
	 */
	static FMantleChassisSpec Reshape(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions);

	/** The width of the circle Spec turns in, cm: twice the wheelbase (from the steered axle to the middle of the others) over the tangent of the steering lock. */
	static double TurningCircleCm(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling);

	/**
	 * What Spec is like, from its handling (derived when shaped, hand-tuned when not). A ground vehicle: weight, turning circle and top speed. A plane:
	 * weight, wing loading, take-off speed, and a top speed that is its air-speed limit. A helicopter: weight and hover throttle. A hovercraft: weight,
	 * hover height and top speed.
	 */
	static FMantleVehicleSummary Summarise(const FMantleChassisSpec& Spec);

	/**
	 * The summary in a creator's words, the numbers of its Family. Ground: "Weighs about 1.6 tonnes. Turns in a circle about 8 m across. Top speed about
	 * 160 km/h." Plane: "Weighs about 7 tonnes. Carries about 350 kg on each square metre of wing. Takes off at about 200 km/h. Top speed about 720 km/h."
	 */
	static FText Describe(const FMantleVehicleSummary& Summary);

	/**
	 * What is worth a creator's attention about Spec, in plain sentences: wheels outside the body, a body that tips over, and every
	 * handling number the derivation held at a limit Mantle allows (FMantleChassisHandling::Clamped: the vehicle will not be quite what its size suggests).
	 */
	static TArray<FText> Warnings(const FMantleChassisSpec& Spec);

	/**
	 * What a handling number the derivation held at a limit means for a creator, in a plain sentence. Path is the document path of the line in
	 * FMantleChassisHandling::Clamped; bHeldDown: the rule gave more than the limit, so the number was held at its most, else at its least. Empty for a
	 * number that does not change how it drives (the camera, the tyres' grip, the wind resistance) and for the centre of mass, which Warnings covers.
	 */
	static FText ClampedSentence(const FString& Path, bool bHeldDown);

	/** A size's name as the window shows it ("Wheelbase", "Wheel radius"), and a sentence on what it is. */
	static FText Label(EMantleVehicleDimension Dimension);
	static FText Help(EMantleVehicleDimension Dimension);

	/** The uniform Size slider's label and sentence, the feel sliders', and the passenger seat's. */
	static FText SizeLabel();
	static FText SizeHelp(EMantleVehicleFamily Family);
	static FText FeelLabel(EMantleVehicleFeel Feel);
	static FText FeelHelp(EMantleVehicleFeel Feel);
	static FText PassengerLabel();
	static FText PassengerHelp();

	/**
	 * What a flier needs to be test-driven, in one plain sentence for the Drive step: a plane or a jet needs a long flat open stretch (about 350 m, about 150 m) or a runway,
	 * a helicopter needs open space above it. Empty for a vehicle that drives or floats.
	 */
	static FText DriveAdvice(const FMantleChassisSpec& Spec);

	/** A Size as text: "1.25x". */
	static FText SizeText(double Size);

	/** A length in words: "85 cm" under a metre, else "3.4 m". */
	static FText LengthText(double Centimetres);

	/** A reason from deeper down as a clause for a sentence of ours: trailing spaces and full stops cut, so the sentence's own full stop is the only one. */
	static FString PlainReason(const FString& Reason);
};

/**
 * The state behind the shape step (#793 step 4), apart from Slate so a test can drive it the way the sliders do: a preset's chassis as the archetype,
 * the eight sizes the creator has set, and what follows from them (the spec, the summary and the warnings). Every slider callback, whether the
 * creator dragged, typed or pressed Reset, comes here, and nothing else changes a size.
 */
class MANTLEEDITOR_API FMantleVehicleShapeSession
{
public:
	/**
	 * Starts from Archetype (a preset at its own size): its own sizes, nothing held. MinSize and MaxSize are the range of the uniform Size a plane,
	 * helicopter or hovercraft is shaped by, the preset's own fit range (`fit.minScale` and `fit.maxScale`); 0 for either uses FMantleChassisSpec's.
	 */
	void Start(const FMantleChassisSpec& InArchetype, double InMinSize = 0.0, double InMaxSize = 0.0);

	bool IsStarted() const { return bStarted; }

	/**
	 * Sets one size to Wanted, held to its range (FMantleVehicleShape::Edit), and rewrites the held notes, the spec, the summary and the warnings
	 * at once. True when a size changed, so the picture needs redrawing; false when it did not (the same size again, or one held where it
	 * was), in which case nothing is recomputed unless the notes changed.
	 */
	bool SetDimension(EMantleVehicleDimension Dimension, double Wanted);

	/** Every size back to the archetype's. True when any changed. */
	bool Reset();

	/** True for a type shaped by one Size (FMantleVehicleShape::UsesSize): it has no eight sizes to move. */
	bool UsesSize() const { return bStarted && FMantleVehicleShape::UsesSize(Archetype); }
	double GetSize() const { return Size; }
	FMantleDimensionRange GetSizeRange() const { return FMantleDimensionRange{ MinSize, MaxSize }; }

	/**
	 * Sets the uniform Size, held to GetSizeRange(); the chassis is the archetype Scaled by it, airframe, rotors, pads and seat together, its handling
	 * derived again. True when the Size changed. A type that is not shaped by Size changes nothing.
	 */
	bool SetSize(double Wanted);

	/**
	 * The feel sliders, each -1 to 1 and 0 for the type's own (FMantleChassisFeel): a ground vehicle (a car, a truck, a six-wheeler, a motorcycle) has all four, a
	 * hovercraft only its top speed, and a plane, jet or helicopter none (their handling is tuned to fly). Setting one holds it to -1..1 and derives the handling again.
	 */
	bool HasFeel(EMantleVehicleFeel Which) const;
	double GetFeel(EMantleVehicleFeel Which) const;
	bool SetFeel(EMantleVehicleFeel Which, double Wanted);

	/** The top-speed feel alone, as the hovercraft had it before the others. */
	bool HasTopSpeedFeel() const { return HasFeel(EMantleVehicleFeel::TopSpeed); }
	double GetTopSpeedFeel() const { return Feel.TopSpeed; }
	bool SetTopSpeedFeel(double Wanted) { return SetFeel(EMantleVehicleFeel::TopSpeed, Wanted); }

	/** Whether the passenger seat is on, and whether the type can have one to turn off (its preset has one). */
	bool HasPassengerOption() const { return bStarted && Archetype.bPassenger; }
	bool GetPassenger() const { return Shaped.bPassenger; }
	bool SetPassenger(bool bWanted);

	/** Everything that makes the vehicle what it is, as text: the sizes, the Size, the feel and the seat. Two equal signatures make the same vehicle. */
	FString Signature() const;

	const FMantleChassisSpec& GetArchetype() const { return Archetype; }
	const FMantleVehicleDimensions& GetDimensions() const { return Dimensions; }
	/** The chassis for the sizes: the archetype itself while unshaped, else resized with its handling derived. */
	const FMantleChassisSpec& GetShaped() const { return Shaped; }
	/** What the last change had to hold to a limit, in plain words; replaced by the next change. */
	const TArray<FText>& GetHeldNotes() const { return HeldNotes; }
	/** "Weighs about 1.5 tonnes. Turns in a circle about 7 m across. Top speed about 160 km/h." */
	const FText& GetSummary() const { return Summary; }
	/** The held notes and the warnings of the spec, one "- " line each; empty for none. */
	const FText& GetWarnings() const { return Warnings; }
	/** True when anything differs from the archetype's own: a size, a feel, the seat or, for a type shaped by Size, the Size. */
	bool IsShaped() const;
	FMantleDimensionRange GetHeldRange(EMantleVehicleDimension Dimension) const { return FMantleVehicleShape::HeldRange(Archetype, Dimensions, Dimension); }

private:
	void Refresh();
	double* FeelField(EMantleVehicleFeel Which);
	/** True when any feel slider is off its middle. */
	bool FeelMoved() const;

	bool bStarted = false;
	FMantleChassisSpec Archetype;
	FMantleVehicleDimensions Dimensions;
	/** The uniform Size and its range, the feel and the seat of a type shaped by Size. */
	double Size = 1.0;
	double MinSize = FMantleChassisSpec::MinScale;
	double MaxSize = FMantleChassisSpec::MaxScale;
	FMantleChassisFeel Feel;
	bool bPassenger = false;
	FMantleChassisSpec Shaped;
	TArray<FText> HeldNotes;
	FText Summary;
	FText Warnings;
};
