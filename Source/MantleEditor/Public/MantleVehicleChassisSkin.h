// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MantleChassisSpec.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleChassisFit.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleShape.h"

class UObject;
struct FMantleVehiclePreset;

/** One body of a model's own collision (a skeletal model's physics asset, a static model's simple collision): the box around it, in the model's own space (#821). */
struct FMantleVehicleModelBody
{
	FBox Box = FBox(ForceInit);
	/** The model's bone the body hangs on; INDEX_NONE for a static model's. */
	int32 Bone = INDEX_NONE;
};

/**
 * A creator's 3D model read for use as a vehicle's skin (#821 WP5, chassis from a model): its first level of detail as plain arrays in the model's own
 * space, its material slots, and, for a skeletal mesh, its bones. A static mesh and a skeletal mesh read alike; a static one simply has no bones.
 * Holds no UObjects beyond the material pointers, so the arithmetic on it is tested without an editor.
 */
struct MANTLEEDITOR_API FMantleVehicleModel
{
	FString Name;
	/** The model is a skeletal mesh: it has bones, and its vertices are weighted to them. */
	bool bSkeletal = false;
	/** LOD 0: positions, normals, UVs, indices and sections as the model has them. Skin weights are those of a skeletal mesh (a static one's are empty). */
	FMantleSkinnedMeshLOD LOD;
	/** The model's material slots, in order. */
	TArray<FSkeletalMaterial> Materials;
	/** A skeletal model's bones, parent first, and where each is in the model's space at its reference pose. */
	TArray<FMantleSkinnedMeshBone> Bones;
	TArray<FVector> BonePositions;
	/**
	 * The model's UV channels above 0 (channel 1 first), each one entry per vertex of LOD, up to the skeletal mesh's limit (4 channels in all), and its painted vertex
	 * colors, one per vertex (empty when it has none, or all white). Carried onto the vehicle's body with the tangents in LOD (the creator's own).
	 */
	TArray<TArray<FVector2f>> ExtraUVs;
	TArray<FColor> Colors;
	/** The model's own collision shapes as boxes around them (an empty list: it has none); the vehicle's collision follows them (ProposeCollision). */
	TArray<FMantleVehicleModelBody> PhysicsBodies;
	/** What reading it had to say in plain words, for the build's notes: a Nanite mesh read from its full-detail source, or from its reduced fallback and why. */
	TArray<FString> Notes;

	/** The box around every vertex, in the model's own space. */
	FBox Bounds() const;
	int32 NumTriangles() const { return LOD.Indices.Num() / 3; }
	int32 FindBone(FName Bone) const;

	/**
	 * Reads Asset (a static mesh or a skeletal mesh). False, with the reason in plain words in OutError, for anything else or a mesh whose geometry cannot be
	 * read here (an editor build always keeps it; a cooked mesh without CPU access does not).
	 */
	static bool Read(UObject* Asset, FMantleVehicleModel& Out, FString& OutError);

	/**
	 * Makes every normal, tangent and UV finite, as the upload's cook requires; Read does it to what it reads. Unreal keeps a mesh's UVs as 16-bit floats unless
	 * told otherwise, so a source UV past 65504 (a broken export's, often in the millions) is read back as infinite. A bad UV takes a good one from another corner
	 * of a triangle it is in (the triangle then samples one texel, as near the creator's as the file allows), else 0; a bad normal the area-weighted normal of
	 * its triangles, else up; a bad tangent one square to its normal. Adds a note to OutNotes per section, in plain words. Returns the vertices changed.
	 */
	static int32 CleanNonFinite(FMantleVehicleModel& Model, TArray<FString>& OutNotes);

	/**
	 * Whether a static mesh is read from its full-detail source and not from its render data (#821). A Nanite mesh's render LOD 0 is its fallback, a much reduced
	 * stand-in; the source model is what the creator made. True (read the source) when the mesh uses Nanite (bNanite), its source is kept (bSourceAvailable) and its
	 * triangles fit a vehicle's cap; false otherwise. OutNote, when given, is set to the plain words the build shows for a Nanite mesh either way, and left empty for a
	 * mesh that is not Nanite. FallbackTriangles is the render LOD 0's count (0: unknown).
	 */
	static bool UseNaniteSource(bool bNanite, bool bSourceAvailable, int32 SourceTriangles, int32 FallbackTriangles, FString* OutNote = nullptr);

	/** A one-line plain description for the window: "a skeletal mesh, 9 bones, 12,000 triangles, 2 materials". */
	FString Describe() const;
};

/** Where on the model one part of the vehicle comes from: one of its bones (and everything weighted to it and its children) or one of its mesh sections. */
struct FMantleVehicleModelPiece
{
	enum class EKind : uint8 { None, Bone, Section };
	EKind Kind = EKind::None;
	/** The model's bone, for Kind Bone. */
	FName Bone;
	/** The model's section (an index into its LOD 0 sections), for Kind Section. */
	int32 Section = INDEX_NONE;

	static FMantleVehicleModelPiece OfBone(const FName InBone)
	{
		FMantleVehicleModelPiece Piece;
		Piece.Kind = EKind::Bone;
		Piece.Bone = InBone;
		return Piece;
	}
	static FMantleVehicleModelPiece OfSection(const int32 InSection)
	{
		FMantleVehicleModelPiece Piece;
		Piece.Kind = EKind::Section;
		Piece.Section = InSection;
		return Piece;
	}
	bool IsSet() const { return Kind != EKind::None; }
	bool operator==(const FMantleVehicleModelPiece& Other) const { return Kind == Other.Kind && Bone == Other.Bone && Section == Other.Section; }
};

/** The parts of a car that open (#546): the six a player can swing. Left is the driver's side (-Y), right the passenger's (+Y). */
enum class EMantleVehicleHingePart : uint8
{
	DriverDoor,
	PassengerDoor,
	RearLeftDoor,
	RearRightDoor,
	Hood,
	Trunk,
};

/**
 * Which of the model's bones is each part that opens, and which of them the creator flipped (a part that swings the wrong way). Partial: a part that is not in Bones does not
 * open. A bone carries its children and everything weighted to them, so a door bone with its handle and window below it opens all of it.
 */
struct FMantleVehicleHingeMap
{
	/** The part to the model's bone it is on. */
	TMap<EMantleVehicleHingePart, FName> Bones;
	/** The parts whose hinge turns the other way (the axis is negated). */
	TSet<EMantleVehicleHingePart> Flipped;

	bool IsEmpty() const { return Bones.Num() == 0; }
	/** The model bone Part is on; none when it is not set. */
	FName BoneOf(const EMantleVehicleHingePart Part) const { const FName* Found = Bones.Find(Part); return Found ? *Found : FName(); }
	bool IsFlipped(const EMantleVehicleHingePart Part) const { return Flipped.Contains(Part); }
};

/**
 * What FindHingesOnModel may use besides the model's own vertices: a car that is a bare skeleton with its doors as separate meshes riding the door bones (the converter's
 * Parts), and a check a converter wants because nobody picked the bones by eye.
 */
struct FMantleVehicleHingeEvidence
{
	/** Boxes, in the model's space, of meshes that ride a bone of the model without being in it: the bone's name and the box. They count for that bone's part and for the body. */
	TArray<TPair<FName, FBox>> Riding;
	/** A part whose box sits on the other side of the car from what its bone's name says (a "left" door on the right, a hood at the rear) is left out and said, not guessed at. */
	bool bCheckPlaces = false;
};

/** The parts of a model the creator has assigned to the chassis's moving bones: the chassis bone's name to where it comes from. Optional, and partial. */
struct FMantleVehicleModelMap
{
	TMap<FName, FMantleVehicleModelPiece> Parts;
	/** The model's doors, hood and trunk: bones that open. Skin gives each its own bone under the chassis's root. */
	FMantleVehicleHingeMap Hinges;

	bool IsEmpty() const { return Parts.Num() == 0; }
	const FMantleVehicleModelPiece* Find(const FName ChassisBone) const { return Parts.Find(ChassisBone); }
};

/** What a chassis bone does, which decides how its part is placed on it. */
enum class EMantleVehiclePartKind : uint8
{
	/** A wheel: it spins about the chassis's wheel center, so the part is moved onto that center. */
	Wheel,
	/** A motorcycle's front fork, or a quad's handlebar (#376 step 9d): it turns with the steering; the part is left where the model has it. */
	Fork,
	/** A propeller or a rotor: it spins about its own hub, so the bone is moved to the part's hub. */
	Spinner,
	/** An aileron, the elevator or the rudder: it turns about its hinge, so the bone is moved to the part's pivot. */
	Surface,
};

/** One moving bone of the chassis a part of the model can be assigned to. */
struct FMantleVehiclePartSlot
{
	FName ChassisBone;
	EMantleVehiclePartKind Kind = EMantleVehiclePartKind::Wheel;
	/** In plain words, for the window: "Left aileron". */
	FText Label;
	/** What moves, in one sentence. */
	FText Help;
};

/** The model turned, scaled and moved into the chassis's space: nose along +X, the ground at Z = 0. */
struct FMantleVehicleModelPlacement
{
	/** Turns the model about Z so its nose points along +X (0, 90, 180 or 270 for a creator's choice; any angle works). */
	double YawDegrees = 0.0;
	double Scale = 1.0;
	/** Added after the turn and the scale. */
	FVector Offset = FVector::ZeroVector;
	/**
	 * What fitting the model had to say in plain words, or empty: its size was held to the scale limits (a model modeled in meters or millimeters), or it was too
	 * small to measure and is left as modeled. The build puts it in its notes, so a clamp is never silent.
	 */
	FString Note;

	FVector Point(const FVector& ModelPoint) const { return FRotator(0.0, YawDegrees, 0.0).RotateVector(ModelPoint) * Scale + Offset; }
	FVector Direction(const FVector& ModelDirection) const { return FRotator(0.0, YawDegrees, 0.0).RotateVector(ModelDirection); }
};

/** How the chassis is sized to a model: a Size for a plane, a jet, a helicopter, a hovercraft or a tracked vehicle, the eight sizes for a road vehicle. */
struct FMantleVehicleModelProposal
{
	/** The type is shaped by one Size (FMantleVehicleShape::UsesSize). */
	bool bUsesSize = false;
	double Size = 1.0;
	/** The sizes to set, in the order to set them, for a road vehicle. */
	TArray<TPair<EMantleVehicleDimension, double>> Dimensions;
	/** True when the type's own limits held the Size back. */
	bool bClamped = false;
	/** Plain words saying what was proposed and why. */
	TArray<FString> Notes;
};

/** One part that opens, worked out from where its vertices and its bone are: the numbers the vehicle's hinge, stand and yaw take, in the body's space, cm. */
struct FMantleVehicleSkinnedHinge
{
	EMantleVehicleHingePart Part = EMantleVehicleHingePart::DriverDoor;
	/** The bone that turns: the model's own bone (its own skeleton), or the bone Skin added for it under the root. */
	FName Bone;
	FVector Pivot = FVector::ZeroVector;
	FVector Axis = FVector::UpVector;
	double Scale = 1.0;
	/** Where the character stands to open it, on the ground, and the way they face (yaw 0 faces +X). */
	FVector Stand = FVector::ZeroVector;
	double Yaw = 0.0;
};

/**
 * A wheel's hub on a model worn as a skin: the model's hub, knuckle or brake bone (hub_lf, a B_FLBrake with the caliper skinned to it), given a bone of its own under the root at
 * the chassis's wheel center, with the model's vertices on it. It is to travel and steer with the wheel and not spin (a linkage rule: FMantleVehicleBuilder::ApplyHubs).
 */
struct FMantleVehicleSkinnedHub
{
	/** The bone Skin added for it (named as the model's bone, suffixed when a chassis bone has the name). */
	FName Bone;
	/** The chassis's wheel bone it follows. */
	FName Wheel;
};

/**
 * A steering wheel measured from the vertices weighted to its bone (FMantleVehicleChassisSkin::MeasureSteeringWheel): where its hub is, how it is turned and how big its rim
 * is, in the same space and units as the points (the body's: nose on +X, cm). bPlaced is false, with Why in plain words, when the measure is not sure.
 */
struct FMantleVehicleSteeringFit
{
	bool bPlaced = false;
	/** The hub: the bone's place. */
	FVector Location = FVector::ZeroVector;
	/** As Tuning.SteeringWheelRotation: X along the column away from the driver, Z towards the top of the rim. */
	FRotator Rotation = FRotator::ZeroRotator;
	/** The rim, where the hands grip it. */
	double Radius = 0.0;
	/** Why nothing was placed, in plain words, for the notes. */
	FString Why;
};

/** The result of putting a model on a chassis as its skin. */
struct FMantleVehicleSkinned
{
	/** The chassis's skeleton with the model's geometry on it, one influence per vertex. */
	FMantleSkinnedMesh Body;
	/** The model's material slots, uniquely named, one per slot the body's sections use. */
	TArray<FSkeletalMaterial> Materials;
	TArray<FString> Notes;
	FMantleVehicleModelPlacement Placement;
	/** The model's other UV channels and vertex colors, in the body's vertex order (vertices copied for a part are copied here too): for FMantleVehicleChassisMesh::Create. */
	FMantleVehicleBodyExtras Extras;
	/** The doors, hood and trunk of the model that open (Map.Hinges), each on a bone of its own under the root, with the vertices of the part weighted to it. */
	TArray<FMantleVehicleSkinnedHinge> Hinges;
	/** The model's hub and brake bones that sit at a wheel Skin moved onto the chassis's: each on a bone of its own under the root, with the vertices of the model bone and its children. */
	TArray<FMantleVehicleSkinnedHub> Hubs;
	/**
	 * The steering wheel, when the model has a bone named like one (SuggestSteeringWheelBone) with vertices weighted mostly to it: the bone Skin gave it in Body (None when it
	 * has none; separate from the hinge bones), and what its vertices measure (Steering; bPlaced false says why not).
	 */
	FName SteeringWheelBone;
	FMantleVehicleSteeringFit Steering;
};

/** Where a proposed collision came from. */
enum class EMantleVehicleCollisionSource : uint8
{
	/** Nothing to follow: the chassis's own box stays. */
	Chassis,
	/** The model's own physics asset or simple collision. */
	PhysicsAsset,
	/** A plane's or jet's body split into fuselage, wings and tail from the model's shape. */
	Airframe,
	/** The chassis's box trimmed to the model, with boxes over its roof and cabin (the same fit a car built from its wheels gets). */
	Hull,
};

/** The collision a vehicle built from a model asks for (ProposeCollision): boxes in the chassis's space, within the document's limits. */
struct FMantleVehicleSkinCollision
{
	/** The largest first; empty when the chassis's own box stays. At most FMantleVehicleLimits::MaxCollisionBoxes, each inside +-1000 cm. */
	TArray<FMantleChassisBox> Boxes;
	EMantleVehicleCollisionSource Source = EMantleVehicleCollisionSource::Chassis;
	/** In plain words, for the build's notes. */
	FString Note;
};

/**
 * Chassis from a model (#821 WP5, #793 step 5): the gray chassis is the physics and the skeleton, and the creator's mesh is its skin. A static or a skeletal
 * model is skinned whole to the chassis's root bone; the creator may assign parts of it to the chassis's moving bones (wheels, propellers, rotors, control
 * surfaces, the fork) so those parts move. An unassigned model still drives or flies; it just does not animate those parts. The chassis is sized to the model
 * (a Size from its footprint, or the eight sizes from its bounds and its assigned wheels), and the creator confirms with the sliders they already have.
 *
 * Pure arithmetic over FMantleVehicleModel and FMantleSkinnedMesh: tested with meshes made in the test.
 */
struct MANTLEEDITOR_API FMantleVehicleChassisSkin
{
	/** True for a propeller plane's chassis with one engine: it may be made a twin (a P-38's two propellers). */
	static bool CanBeTwin(const FMantleChassisSpec& Spec);

	/**
	 * Spec as a twin-engined plane (#821): two engines of half the thrust each, so the same total, and a propeller bone each (propeller_l and propeller_r) on the
	 * wings' thirds, which the creator's two propellers can ride. Unchanged when CanBeTwin is false.
	 */
	static FMantleChassisSpec AsTwin(const FMantleChassisSpec& Spec);

	/**
	 * How many propellers Model has: bones with "prop" in the name that are not a guard, a chain of them (prop, prop_blade, prop_end) counted once, the way SuggestMap
	 * takes them. Two is a twin-engined plane.
	 */
	static int32 CountPropellers(const FMantleVehicleModel& Model);

	/** The moving bones of Chassis (Spec's gray body) that a part of the model can be assigned to, in a sensible order: wheels, fork, surfaces, propellers, rotors. */
	static TArray<FMantleVehiclePartSlot> PartSlots(const FMantleChassisSpec& Spec, const FMantleSkinnedMesh& Chassis);

	/**
	 * Parts of Model that obviously belong to Slots, by name and by where they are once the model faces Yaw: a bone with "propeller" in its name, a "tail rotor", a
	 * left and a right aileron, a wheel under each wheel of the chassis, a front fork. Nothing is assigned on a guess that does not fit (three wheels for four slots).
	 */
	static FMantleVehicleModelMap SuggestMap(TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModel& Model, double YawDegrees);

	/**
	 * SuggestMap at each of the four turns (0, 90, 180, 270 degrees), keeping the first at which the model's front is ahead of its rear: a front wheel before a rear
	 * wheel, or the main rotor before the tail rotor, along +X. Returns that turn and puts its map in OutMap. A model with no such pair keeps the way it was
	 * modeled (0) and gets the map for that.
	 */
	static double SuggestMapAndYaw(TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModel& Model, FMantleVehicleModelMap& OutMap);

	/** The model's length (X) and width (Y) once it faces Yaw, and its height (Z). */
	static FVector Extent(const FMantleVehicleModel& Model, double YawDegrees);

	/**
	 * How the chassis of Archetype should be sized to Model facing Yaw: a Size from the footprints' ratio for a type shaped by Size, else the eight sizes
	 * (the body from the model's bounds, the wheels from the assigned wheel parts when there are some, otherwise the preset's proportions at the model's length).
	 */
	static FMantleVehicleModelProposal Propose(const FMantleChassisSpec& Archetype, const FMantleVehiclePreset& Preset, const FMantleVehicleModel& Model, double YawDegrees,
		const FMantleVehicleModelMap& Map);

	/** Turns, scales and moves Model onto Spec's chassis (Chassis is its gray body): the scale fits the footprint (a flier, a hovercraft, a tracked vehicle) or the length (a road vehicle). */
	static FMantleVehicleModelPlacement FitPlacement(const FMantleChassisSpec& Spec, const FMantleSkinnedMesh& Chassis, const FMantleVehicleModel& Model, double YawDegrees,
		const FMantleVehicleModelMap& Map);

	/**
	 * Puts Model on Chassis as its skin with Placement: every vertex weighted whole to the chassis bone its part is assigned to, else to the root. A bone that
	 * spins or turns about its hub or hinge (a propeller, a rotor, a control surface) is moved to the part's own, and a wheel's part is moved onto the chassis's
	 * wheel center when it is within MaxWheelMove cm of it (a wheel further off stays on the body, still, and is said so: moving it would tear it from its strut,
	 * and turning it about the chassis's wheel would swing it round that point). Each door, the hood and the trunk in Map.Hinges whose bone the model has gets a new bone
	 * under the root, last in the table (named as the model's bone, suffixed when a chassis bone has the name), posed at the part's hinge, with the vertices of that model bone and
	 * its children weighted to it; the hinges are in Out.Hinges (HingeFor). Each hub or brake bone of the model (FMantleVehicleConverter::IsHubBoneName, IsBrakeBoneName) at a
	 * wheel that was moved onto the chassis's (within 25 cm of the model's wheel center) gets a bone of its own the same way, at the chassis's wheel center, with its
	 * vertices and its children's moved with that wheel; they are in Out.Hubs, to follow the wheel's steering and travel without its spin. The sections and material slots are the model's (named uniquely). False, with the
	 * reason in OutError, when the model has nothing to skin.
	 */
	static bool Skin(const FMantleSkinnedMesh& Chassis, TConstArrayView<FMantleVehiclePartSlot> Slots, const FMantleVehicleModel& Model,
		const FMantleVehicleModelPlacement& Placement, const FMantleVehicleModelMap& Map, FMantleVehicleSkinned& Out, FString& OutError, double MaxWheelMove = 20.0);

	/**
	 * The collision a model gets (#821): shaped like the model and not the gray chassis's box. Taken from, in order, the model's own physics asset (box, sphere, capsule
	 * and convex bodies, except those of wheels, propellers and rotors, merged down to the document's box count), else, for a plane or a jet, an airframe split from the
	 * placed model's surface (fuselage, wings, tail), else, for everything else, the chassis's box trimmed to the model with boxes over its roof and cabin. Every box is
	 * kept inside the document's +-1000 cm and no lower than the chassis's own box, so a gear or a wheel the model draws below the body never drags on the ground.
	 * Body is the skinned result of Skin (the model placed on the chassis), Slots its moving parts.
	 */
	static FMantleVehicleSkinCollision ProposeCollision(const FMantleChassisSpec& Spec, const FMantleSkinnedMesh& Body, TConstArrayView<FMantleVehiclePartSlot> Slots,
		const FMantleVehicleModel& Model, const FMantleVehicleModelPlacement& Placement, const FMantleVehicleModelMap& Map);

	/**
	 * At most MaxBoxes boxes that together hug the surface made of Indices' triangles over Positions (units as given): the surface is sampled into a coarse grid and cut,
	 * largest empty volume first, along the plane that leaves the least empty box around the parts. A plane comes out as a fuselage, a wing on each side and a tail; a
	 * single box stays one. Pure and deterministic. Every vertex of the triangles lies inside one of the boxes.
	 */
	static TArray<FBox> BoxesAroundSurface(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, int32 MaxBoxes);

	/** Merges Boxes down to MaxBoxes, each time the two whose union adds the least volume (of more than 64, the smallest are dropped first). */
	static TArray<FBox> ReduceBoxes(TArray<FBox> Boxes, int32 MaxBoxes);

	/** The six parts that open, in the order the window lists them. */
	static TConstArrayView<EMantleVehicleHingePart> HingeParts();

	/** Whether Part is a door (the four of them), not the hood or the trunk. A vehicle with no inside leaves every door shut and still opens the other two. */
	static bool IsDoorPart(EMantleVehicleHingePart Part) { return Part != EMantleVehicleHingePart::Hood && Part != EMantleVehicleHingePart::Trunk; }

	/** A part's name in plain words, for the window and the notes: "Driver door", "Hood". */
	static FText HingeLabel(EMantleVehicleHingePart Part);

	/**
	 * The parts of Model that obviously open, by the names of its bones (any case): GTA's door_dside_f (driver), door_pside_f (passenger), door_dside_r (rear left),
	 * door_pside_r (rear right), bonnet (hood) and boot (trunk), and the usual door_fl / door_fr / door_rl / door_rr (also door_lf and the like), hood, trunk and
	 * tailgate. A name is read in words (split at "_", "-", spaces, dots and where a capital starts one: B_SUV04_FLDoor is b, suv, 04, fl, door): a door is a "door" word with a
	 * side and a row (fl, lf, front_left, dside_f, door_l and so on; a door with a side and no row is a front door), a hood is "hood" or "bonnet", a trunk is "boot", "trunk",
	 * "tailgate", "liftgate" or "hatch", or a "door" that is "rear" or "back" with no side (the SUV's B_SUV04_RearDoor). A bone with a handle, window, hinge or the like in its
	 * name is part of one, not the part, and doorhandle (one word) is not a door. A static model, or one with none of these, gives an empty map. Nothing is flipped.
	 * OutNotes, when given, says in plain words which door-looking bones were not taken and why.
	 */
	static FMantleVehicleHingeMap SuggestHinges(const FMantleVehicleModel& Model, TArray<FString>* OutNotes = nullptr);

	/**
	 * Whether a bone's name says it is the steering wheel. Read in words as SuggestHinges reads them: "steering" alone or followed by "wheel" (Steering_Wheel, steering_wheel,
	 * B_SteeringWheel, steering), the one word "steeringwheel" (GTA V's standard name for it, from its published bone list, not checked against a file here), or a whole
	 * underscore part "SW" in any case with at least one other part (B_SUV01_SW, B_NYC_Car02_SW). "Sw" inside another word (swing_arm) or alone is not it.
	 */
	static bool IsSteeringWheelBoneName(const FString& BoneName);

	/**
	 * The model's bone that is the steering wheel, by its name (IsSteeringWheelBoneName), or none. Only a bone inside the cabin counts: within the model's bounds and above the
	 * lowest third of its height, and, when Seat is given (the driver's seat, body's space, nose on +X), in front of it by at most 120 cm, so a stray part elsewhere never wins.
	 * Of several, the one nearest the driver (the seat's side and just ahead of it; with no seat, the driver's side, -Y). The root and the bones in Avoid (the wheels' bones and
	 * what they hang from, a door's) are never taken. A static model gives none.
	 */
	static FName SuggestSteeringWheelBone(const FMantleVehicleModel& Model, const FVector* Seat, TConstArrayView<FName> Avoid);

	/** The model's vertices (in its own space) whose heaviest weight is on Bone: what that bone carries, with none of its children's. */
	static TArray<FVector> VerticesOnBone(const FMantleVehicleModel& Model, int32 Bone);

	/**
	 * Fits a steering wheel to Points, the vertices weighted to its bone, with Hub the bone's place. Nose on +X, Z up. The wheel's plane is the one the points are flattest
	 * about (the smallest spread of the three axes is the column), the column pointing forward; the rim is the 90th percentile of the points' distance from the column through
	 * Hub. Sure only when there are enough points, they are clearly flat and round, a good share of them lie on the rim, the hub is near their middle, the face looks along the
	 * car and the rim is 10 to 30 cm (the vehicle asset's own range); otherwise bPlaced is false and Why says which.
	 */
	static FMantleVehicleSteeringFit MeasureSteeringWheel(TConstArrayView<FVector> Points, const FVector& Hub);

	/**
	 * Where Part's hinge goes, from the box around its vertices and where its bone is (the pure part of Skin, also used for a model that keeps its own skeleton), all in the
	 * body's space. PartBox is the box around the part's vertices, BodyBox the box around the whole body, BonePosition the bone's place when it is known. A door turns about
	 * its front edge (the bone's place when it is within a quarter of the door's length from the front, else the front edge at the outer side, halfway up) on an upright
	 * axis; the hood about its rear edge and the trunk about its front edge, at the top, on a sideways one. A trunk whose bone sits at one side edge (BoneAtEdge on Y) and
	 * not at its top is a side-hinged tailgate: it turns on an upright axis through the bone, swinging out away from the car's middle (+Z hinged on -Y, -Z on +Y), still
	 * opened from behind the car. An upright trunk (taller than deep) whose bone sits at its bottom edge (BoneAtEdge on Z), not at a side or its top, is a pickup's drop-down tailgate: it turns about
	 * -Y through the bone, its top swinging back and down. Flipped turns the axis the other way. Doors turn at 0.75, lids
	 * at 1. The character stands on the ground (Z = 0) 60 cm out from the body: at the door's side halfway along it, in front of the hood, behind the trunk, facing the car.
	 * Every number is held to the document's +-1000 cm.
	 */
	static FMantleVehicleSkinnedHinge HingeFor(EMantleVehicleHingePart Part, FName Bone, const FBox& PartBox, const FBox& BodyBox, const FVector* BonePosition, bool bFlipped);

	/** Whether a hinge bone at Bone (one axis) sits at a part's Edge on that axis: within a quarter of the part's Span (its size on that axis). Doors and tailgates use it. */
	static bool BoneAtEdge(double Bone, double Edge, double Span);

	/**
	 * The hinges of a model that keeps its own skeleton, or the whole model placed (Placement): for each part in Hinges whose bone Model has (not its root), the vertices
	 * that bone and its children carry (the heaviest weight decides, a nearer mapped bone takes its own) give the box, and HingeFor the numbers. A part with no bone in
	 * the model, or no vertices on it, is left out and OutNotes says so in plain words. Evidence adds the boxes of meshes riding the bones, and can hold each part to its side of the car.
	 */
	static TArray<FMantleVehicleSkinnedHinge> FindHingesOnModel(const FMantleVehicleModel& Model, const FMantleVehicleModelPlacement& Placement, const FMantleVehicleHingeMap& Hinges,
		TArray<FString>* OutNotes = nullptr, const FMantleVehicleHingeEvidence* Evidence = nullptr);

	/** Plain words for a piece of the model, as the window's pickers list it: "bone FR" or "section 2: Glass". */
	static FString DescribePiece(const FMantleVehicleModel& Model, const FMantleVehicleModelPiece& Piece);

	/** Every piece of Model that can be picked: its bones (except the root), then its sections. */
	static TArray<FMantleVehicleModelPiece> Pieces(const FMantleVehicleModel& Model);
};
