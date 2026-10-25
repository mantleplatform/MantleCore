// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"
#include "MantlePlaceValidator.h"

class FJsonObject;
class UActorComponent;
class UMaterialInterface;
class UMantleVehicleSource;
class UPhysicsAsset;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMesh;
struct FMantleVehicleSourceBox;
struct FMantleVehicleSourceLinkage;
struct FMantleVehicleSourcePart;
struct FMantleVehicleSourceSeatContacts;
struct FMantleVehicleSourceTuning;
struct FReferenceSkeleton;

/**
 * A static mesh found riding a vehicle's body (#435 item 2): attached to AttachTo (a socket or bone of the body; None for the
 * body itself) at Relative, which is its placement relative to that point.
 */
struct MANTLEEDITOR_API FMantleVehicleAttachedMesh
{
	const UStaticMesh* Mesh = nullptr;
	FName AttachTo;
	FTransform Relative;
	/** Per-slot overrides (a component's), by slot index; null keeps the mesh's own. */
	TArray<UMaterialInterface*> Overrides;
	/** Where it was found, for the notes: the component's name, or "preview attachment". */
	FString Label;
};

/** A ChaosVehicles movement component's wheel setup: the bone it names and its wheel class's sizes (0 where it has none). */
struct MANTLEEDITOR_API FMantleVehicleWheelSetup
{
	FName Bone;
	/** WheelRadius and WheelWidth, cm. */
	float Radius = 0.f;
	float Width = 0.f;
	/** MaxSteerAngle, degrees, for a wheel affected by steering; 0 for one that is not. */
	float MaxSteerAngle = 0.f;
};

/**
 * What Convert Vehicle finds around a body besides the mesh (#435): the static meshes riding it, the physics asset its
 * chassis simulates on, and a vehicle movement component's centre-of-mass override, mass and wheel setups.
 */
struct MANTLEEDITOR_API FMantleVehicleSurroundings
{
	TArray<FMantleVehicleAttachedMesh> Attached;

	/** The body's physics asset: a component's override, else the mesh's own. Null: none. */
	const UPhysicsAsset* PhysicsAsset = nullptr;

	/** A ChaosVehicles movement component's bEnableCenterOfMassOverride and CenterOfMassOverride (the root body's space). */
	bool bCenterOfMassOverride = false;
	FVector CenterOfMassOverride = FVector::ZeroVector;

	/** The movement component's Mass, kg (0: none), and its WheelSetups: Convert's defaults for the tuning's mass and steering and each wheel's size. */
	float Mass = 0.f;
	TArray<FMantleVehicleWheelSetup> WheelSetups;

	/** Where they came from, for the notes ("actor BP_OffroadCar", "SKM_Offroad's preview attachments"). */
	FString From;
};

/** A wheel-centre bone Convert Vehicle recognised, and which corner of the car it is. */
struct MANTLEEDITOR_API FMantleVehicleWheelBone
{
	FName Bone;
	/** 0 front, 1 middle, 2 back. */
	int32 Axle = 0;
	bool bLeft = false;
	/** The name has no side: a wheel on the centre line (wheel_f and wheel_r, a motorcycle's two or a plane's nose wheel). */
	bool bCentre = false;
};

/** What Convert Vehicle made of a skeletal mesh, or why it could not. */
struct MANTLEEDITOR_API FMantleVehicleConversion
{
	bool bSuccess = false;

	/** What stops the conversion, each sentence ending in what to do. Nothing is made while there are any. */
	TArray<FString> Problems;

	/** What Mantle chose or measured, and what to look at before publishing. */
	TArray<FString> Notes;
};

/**
 * Creator-published vehicles (#435), the plugin side: the Convert Vehicle command and the upload a Mantle Vehicle
 * Spawn's vehicle becomes.
 *
 * The recipe's naming (the Vehicle template's PhysWheel_FL and Phys_Wheel_BR): a wheel-centre
 * bone's name contains "wheel" (any case) and ends in its corner, "_FL"/"_FR" front, "_ML"/"_MR" middle, "_BL"/"_BR"
 * or "_RL"/"_RR" back. The word naming a common free car pack uses is read too: words split on "_", one containing
 * "wheel", a position word (front; middle or mid; rear or back) and a side word first or last (l or left, r or right), as
 * in wheel_front_l or left_rear_wheel. A wheel-named bone that is a helper (a word with turn, steer, pivot, hub, knuckle,
 * upright, spindle or carrier, or "no spin") is not a wheel; a wheel hanging from a steering pivot of its corner makes the
 * pivot the wheel (FindWheelBones). A centre-line wheel has no side: "wheel" and f or front, r or rear or back (wheel_f, wheel_rear). Convert Vehicle finds those bones, makes front wheels steer and back wheels drive and hold the
 * handbrake (middle wheels drive), measures each wheel from the geometry weighted to its bone (as the cook does), and
 * writes a UMantleVehicleSource with the schema's default tuning for the creator to adjust. It refuses, saying what
 * to rename or fix, when it finds fewer than two or more than eight wheels, a skeleton past the body's bone cap, or a
 * mesh it cannot read.
 *
 * Publishing uploads the vehicle as one GLB: the body's LOD0 with any separate tire meshes merged onto their wheel
 * bones, and the vehicle document (schema/vehicle.schema.json) at extras.mantleVehicle, its materials the uploaded
 * ids of the body's own. The cook worker checks it again with the same numbers (limits.json placeAssets.vehicle).
 * Its attached parts (#435 item 2) upload before it as meshes, with their materials, and the document names their ids.
 */
class MANTLEEDITOR_API FMantleVehicleConverter
{
public:
	/** Where the document rides in the GLB (backend cook.VehicleExtrasKey). */
	static const TCHAR* ExtrasKey;

	/** Vertices weighted to a wheel bone alone that a measurement needs (limits.json placeAssets.vehicle.wheelMinVertices). */
	static constexpr int32 WheelMinVertices = 8;

	/** Where a transmission shifts up and down, as fractions of Max RPM, when the source leaves Change Up / Change Down RPM at 0. */
	static constexpr double DefaultChangeUpFraction = 0.85;
	static constexpr double DefaultChangeDownFraction = 0.4;

	/** The index of the joint named Bone in Body (FName compares ignoring case, as the cook's bone keys do), or INDEX_NONE. */
	static int32 JointIndex(const FMantleGlbMesh& Body, FName Bone);

	/** "vehicle:" + the source's object path: the local key the converter, the upload queue and the exporter share. */
	static FString VehicleKey(const UMantleVehicleSource* Source);

	/** ast_u and 32 lowercase hex digits: an uploaded asset's id (a spawn's VehicleId from a place file). */
	static bool IsUploadedId(const FString& Id);

	/** The recipe's naming, or the word naming, for a wheel-centre bone (see the class comment). False when Bone is not one, or is a wheel's helper. */
	static bool ParseWheelBoneName(const FString& Bone, FMantleVehicleWheelBone& Out);

	/**
	 * Every wheel-centre bone of Skeleton, front to back, left before right. A wheel-named bone at the same place as
	 * another of its corner, hanging from something other than the root while the other hangs from the root, is a
	 * visual copy of it (the Vehicle template's VisWheel_FL under its hub, beside PhysWheel_FL): the root's child is
	 * the wheel, and DetectLinkage copies the other onto it. A wheel hanging from a steering pivot of its corner (wheel_front_l
	 * under wheel_front_turn_l) is carried by it, and no linkage may move a bone a wheel hangs from: the pivot is the wheel (so
	 * it steers and spins, as a ChaosVehicles wheel setup names it), and DetectLinkage gives the named one its travel and steering.
	 */
	static TArray<FMantleVehicleWheelBone> FindWheelBones(const FReferenceSkeleton& Skeleton);

	/**
	 * A bone name's corner as the recipe's naming spells it ("_fl", "_fr", "_ml", "_mr", "_bl", "_br"; "_rl"/"_rr" read as "_bl"/"_br"),
	 * or as the word naming does (wheel_rear_no_spin_l is "_bl"); empty for none.
	 */
	static FString CornerOf(const FString& Bone);

	/**
	 * Suspension linkage for a body (#435 b), from its bone names and where they sit in the reference pose, the way the
	 * Vehicle template's Control Rigs move theirs (the plugin reads no Control Rig: MantleCore takes no plugin
	 * dependency):
	 *   - a hub (hub, knuckle, upright, spindle or carrier, ending in a wheel's corner), or a wheel's helper of its corner
	 *     beside it (wheel_rear_no_spin_l), copies its wheel's travel and steering (the rotation's twist about Z), not its
	 *     spin; so does the wheel-named bone hanging from a wheel that is a steering pivot (wheel_front_l);
	 *   - a bone named like a wheel at a wheel's place (a visual wheel) copies the wheel whole;
	 *   - an axle or beam at a wheel's place copies its travel and aims at the other wheel on its axle;
	 *   - a corner bone with a child sitting on a bone that already moves (the arm's end on a hub) aims at that bone:
	 *     control arms, tie rods, links; repeated, so a link on an arm follows the arm;
	 *   - a damper (damper, shock, strut, coilover or spring, with a corner) whose child sits on a moving bone stretches
	 *     from its top to it.
	 * "Sits on" is within 2 cm, or a tenth of the bone's length. OutNotes gets what was found and the corner bones left
	 * in their reference pose. Rules never move the root, a wheel bone or a bone a wheel hangs from.
	 */
	static TArray<FMantleVehicleSourceLinkage> DetectLinkage(const FReferenceSkeleton& Skeleton, TConstArrayView<FName> WheelBones, TArray<FString>& OutNotes);

	/**
	 * Measures a wheel from Body's geometry: the vertices weighted to Bone alone, radius half the larger of their X and
	 * Z extent, width their Y extent (the cook's rule). False when fewer than WheelMinVertices are.
	 */
	static bool MeasureWheel(const FMantleGlbMesh& Body, FName Bone, double& OutRadius, double& OutWidth);

	/** Fills Source from Mesh: its wheels (the recipe's naming), measured, and the schema's default tuning; Mesh's own surroundings (SurroundingsOfMesh). */
	static FMantleVehicleConversion Convert(USkeletalMesh* Mesh, UMantleVehicleSource& Source);

	/**
	 * Convert, with what surrounds the body (#435): its attached meshes become Source's Parts when it has none (the one shaped like
	 * a wheel's tire becomes that wheel's Tire Mesh when it has none: AddParts), and the chassis collision and centre of mass are
	 * read from its physics (ReadChassisPhysics) when the tuning sets neither. A movement component's wheel setups size the wheels
	 * that have no size of their own, and its mass and steering angle replace the tuning's while those are the defaults.
	 */
	static FMantleVehicleConversion Convert(USkeletalMesh* Mesh, UMantleVehicleSource& Source, const FMantleVehicleSurroundings& Around);

	/**
	 * Convert, into <Mesh>_Vehicle beside the mesh (made, or updated keeping its tuning, parts and the collision and centre of
	 * mass it has), left unsaved for the creator to look at. Around defaults to the mesh's own surroundings. Null, with
	 * OutResult saying why, when the mesh will not convert.
	 */
	static UMantleVehicleSource* ConvertAsset(USkeletalMesh* Mesh, FMantleVehicleConversion& OutResult, const FMantleVehicleSurroundings* Around = nullptr);

	/** A skeletal mesh's own surroundings: its preview attachments (Persona's Add Preview Asset, on a socket or bone) and its physics asset. */
	static FMantleVehicleSurroundings SurroundingsOfMesh(const USkeletalMesh& Mesh);

	/**
	 * A body component's surroundings (a level actor's vehicle, or a ChaosVehicles pawn placed in a level): every static mesh
	 * component among Components attached under Body (through any scene components between, their offsets folded in), the
	 * physics asset Body simulates on (its override, else its mesh's), and a movement component's centre-of-mass override.
	 */
	static FMantleVehicleSurroundings SurroundingsOfComponent(const USkeletalMeshComponent& Body, TConstArrayView<const UActorComponent*> Components);

	/**
	 * The chassis's collision boxes and centre of mass from a physics asset (#435): every body not on a wheel's bone or under
	 * one, each shape's axis-aligned box in the body's component space (a turned shape widened to its bounds; more than
	 * FMantleVehicleLimits::MaxCollisionBoxes merged into one around them all), and the centre as the shapes' volume-weighted
	 * centres plus each body's COM nudge. False, with a note, when there is no chassis body or its boxes fall outside the ranges.
	 */
	static bool ReadChassisPhysics(const UPhysicsAsset& Physics, const FReferenceSkeleton& Skeleton, TConstArrayView<FName> WheelBones,
		TArray<FMantleVehicleSourceBox>& OutBoxes, FVector& OutCenterOfMass, TArray<FString>& OutNotes);

	/**
	 * #435 item 2: the found meshes as Source's parts, each on its socket's bone (or its bone, or the root) with the socket's offset
	 * folded in, in the bone's space. A wheel with no Tire Mesh takes the widest mesh shaped like its tire (round across X and Z,
	 * narrower in Y, centred on its spin axis, within a fifth of twice its radius when that is known: as set, else MeasuredRadii),
	 * hung from the root, its bone or a bone of its corner (a tire modelled in car space, or on a steering pivot), re-expressed about
	 * its bone; failing any, the first mesh on its bone or on a bone within 1 cm of it with the same corner. Other tire-shaped meshes
	 * ride the wheel's bone and spin with it; any other mesh on a wheel's bone (a brake caliper) moves to its hub, which steers
	 * without spinning, or the root. Parts are added only when Source has none; its notes say what was done.
	 */
	static void AddParts(const USkeletalMesh& Mesh, TConstArrayView<FMantleVehicleAttachedMesh> Attached, UMantleVehicleSource& Source,
		TArray<FString>& OutNotes, const TMap<FName, double>& MeasuredRadii = TMap<FName, double>());

	/** The material a part's slot publishes with: its override, else the mesh's own slot material (FMantlePlaceAssets::MeshSlotMaterial). */
	static UMaterialInterface* PartSlotMaterial(const FMantleVehicleSourcePart& Part, const UStaticMesh* Mesh, int32 Slot);

	/**
	 * Whether a material reads as a car's paint, for Convert Vehicle's suggestion (a body section's or a part slot's): Mantle's
	 * car paint (FMantlePlaceAssets::PublishesAsCarPaint, Clear Coat) or a vehicle paint material (a "Paint Tint" vector
	 * parameter, the built-in cars' paint contract).
	 */
	static bool LooksLikePaint(const UMaterialInterface* Material);

	/**
	 * #435 item 2: the slots of Part's mesh to paint, by the mesh's slot index: each upload slot whose material LooksLikePaint or
	 * is one of BodyPaintMaterials (the painted body sections' own), so a shell drawn with the body's paint wears it too.
	 */
	static TArray<int32> SuggestPartPaintSlots(const FMantleVehicleSourcePart& Part, TConstArrayView<const UMaterialInterface*> BodyPaintMaterials);

	/** A part's paint slots as its upload counts them (#435 item 2): each mesh slot's place in FMantlePlaceAssets::UploadSlots; ones not there are left out. */
	static TArray<int32> PartUploadPaintSlots(const FMantleVehicleSourcePart& Part, const UStaticMesh* Mesh);

	/**
	 * The body as uploaded: LOD0 (render vertices in order, the raw reference skeleton, float weights) with each
	 * wheel's tire mesh merged onto its bone, weighted to it alone. OutMaterials[i] is glTF material i's.
	 */
	static bool ReadBody(const UMantleVehicleSource& Source, FMantleGlbMesh& OutBody, TArray<UMaterialInterface*>& OutMaterials, FString& OutError,
		bool bApplyFitScale = false);

	/**
	 * The vehicle as it exports (#728): Source with its Size (FitScale) applied, so every consumer sees the same scaled vehicle. Returns
	 * Source itself, unchanged, when its FitScale is 1; otherwise a transient copy with FitScale 1 and every geometric number multiplied by it.
	 * Geometric (scaled): a wheel's Radius, Width and TireTransform (location and scale); the centre of mass; the seat, passenger seat,
	 * steering wheel (location and radius), hinge pivots (doors, trunk lid), trunk stand, every contact point and exit location, every
	 * collision box's centre and extent, and a part's location and scale (a part rides the scaled body, so it grows with it). Everything
	 * else is tuning or not a length and keeps its value: mass, speeds, drag, engine, gears, suspension travel and springs, steering angle,
	 * brakes, friction, camera arm and height, rotations, axes, a hinge's Scale lever, yaws, paint, linkage, ids and names. The body's own
	 * geometry is scaled by ReadBody (bApplyFitScale), which must be given the original, not this copy. The copy is not rooted: hold it
	 * (TStrongObjectPtr) across any call that can collect garbage.
	 */
	static const UMantleVehicleSource* MakeExportView(const UMantleVehicleSource& Source);

	/**
	 * #612: first-guess contact points for a placed seat (the driver's, or the passenger's when bPassenger), from what Convert
	 * already has: the pelvis 34 cm under the seat's location and 4.3 cm ahead (the default avatar seated in the pack sedan); the
	 * driver's hands at ten and two on the Steering Wheel's rim (kind Wheel, turned as the hub), or else on the lap (Fixed); the
	 * balls of the feet 80 cm ahead, 70 cm down and 12 cm either side of the seat's middle; and an exit on each side, the seat's
	 * own first, as far out as the client's exit spot (the collision's side plus 1 m, at least 2.2 m) on the collision's floor.
	 * Every point in the vehicle's own space, within the document's ranges. Convert fills a placed seat that has none; the
	 * contact-point gizmo starts from them.
	 */
	static FMantleVehicleSourceSeatContacts GuessSeatContacts(const FMantleVehicleSourceTuning& Tuning, bool bPassenger);

	/**
	 * The vehicle document for Source: schema/vehicle.schema.json, keys in the schema's order, numbers rounded, only
	 * what the creator set away from "measure it" (a wheel's radius and width, the seat, the shift points, the
	 * collision). SectionMaterialIds maps a body section (glTF material name) to its uploaded material.
	 *
	 * OutError, when given, receives why the vehicle's movement type, seat kinds and movement blocks (FMantleVehicleChassisDocument::MergeSource) could not be written
	 * into the document, in plain words, and is left empty when they were. A document whose movement blocks do not read is missing them (a bike without its rider block),
	 * so the check (CheckSource) and the build ask, and say so, rather than let it export.
	 */
	static TSharedRef<FJsonObject> MakeDocument(const UMantleVehicleSource& Source, const TMap<FString, FString>& SectionMaterialIds, FString* OutError = nullptr);

	/**
	 * A copy of Document with its materials set (sections sorted, after the collision, as MakeDocument writes them); none
	 * when the map is empty. The paint and liveries keep their place after it.
	 */
	static TSharedRef<FJsonObject> WithMaterials(const TSharedRef<FJsonObject>& Document, const TMap<FString, FString>& SectionMaterialIds);

	/** A copy of Document with its liveries set (#488): the uploaded texture ids in the source's order, in the schema's place; none when empty. */
	static TSharedRef<FJsonObject> WithLiveries(const TSharedRef<FJsonObject>& Document, const TArray<FString>& LiveryIds);

	/**
	 * One part as its upload names it (#435 item 2): the uploaded mesh's id, the uploaded material id of each slot ("" for the
	 * default surface), and the slots (in the upload's order) that wear the vehicle's paint.
	 */
	struct FPartIds
	{
		FString Mesh;
		FName Bone;
		FTransform Transform;
		TArray<FString> Materials;
		TArray<int32> PaintSlots;
	};

	/** A copy of Document with its parts set (#435 item 2), last, as the schema orders them; none when empty. */
	static TSharedRef<FJsonObject> WithParts(const TSharedRef<FJsonObject>& Document, const TArray<FPartIds>& Parts);

	/**
	 * A copy of Document with its horn set (#546): the uploaded sound's id (the source's Horn Sound, uploaded before the vehicle as a
	 * SoundEmitter's sound is), after the parts; none when empty.
	 */
	static TSharedRef<FJsonObject> WithHorn(const TSharedRef<FJsonObject>& Document, const FString& HornSoundId);

	/**
	 * The body's section names as they upload: its glTF material names, each cut to 200 characters, an empty one named
	 * "material_<index>", and a repeat given "_2", "_3" and so on, so each is unique. A vehicle document's materials and paint
	 * sections name these.
	 */
	static TArray<FString> UniqueSectionNames(const TArray<FString>& MaterialNames);

	/**
	 * The local keys Source's liveries (#488) upload under, in its order: each is gathered and converted as a decal's base
	 * colour is (FMantlePlaceAssets::DecalTextureKey), alpha kept. Empty entries for liveries that do not load.
	 */
	static TArray<FString> LiveryTextureKeys(const UMantleVehicleSource& Source);

	/** The upload: Body with Document at extras.mantleVehicle. */
	static bool WriteUpload(const FMantleGlbMesh& Body, const TSharedRef<FJsonObject>& Document, TArray<uint8>& OutBytes, FString& OutError);

	/**
	 * Check this place's rules for one vehicle a spawn uses: a body with every wheel bone (not the root), 2 to 8
	 * wheels with one steering and one driven, every number within limits.json's ranges, each wheel measurable (or
	 * given a radius and width), and the body within placeAssets.vehicle.body's caps. Its paint on sections the body
	 * has, and its liveries (#488) within placeAssets.livery: at most 8, each loading, on paint, laid out on UV 0 (a
	 * published body carries one UV set), the default one of them; a livery without alpha, or past the texture size
	 * rule, is a warning. Its linkage (#435 b): at most 64 rules, each with its type's bones, all in the body, none moving
	 * the root, a wheel bone or a bone a wheel hangs from, none aiming along no direction. Its parts (#435 item 2): at most 16,
	 * each a mesh of the project (not a library one) that loads, on a bone of the body, placed within the ranges without an axis
	 * scaled to nothing, with at most one material per slot. Its horn (#546): a sound wave of the project (not a library one) that
	 * loads and passes FMantlePlaceAssets::CheckSoundRule. Its seats' contact points (#612): within the ranges and their seat's
	 * reach, a driver's steering-wheel hands on the rim and on no bone, no point on a wheel's bone or on one the body lacks, at
	 * most 4 exits a seat. UsedBy names the spawns.
	 */
	static void CheckSource(const UMantleVehicleSource* Source, const FString& UsedBy, TArray<FMantleValidationIssue>& OutIssues);
};
