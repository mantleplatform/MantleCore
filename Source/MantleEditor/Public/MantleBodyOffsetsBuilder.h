// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMetaHumanCharacter;
class USkeletalMesh;

/**
 * #849 S6a: one body-shape offset (weight+, muscle-, ...) in the plugin's own space: Unreal axes, centimetres,
 * the converter's built body. FMantleGlbWriter converts it to glTF and writes it in the root extension
 * MANTLE_body_offsets (backend cook/bodyoffsets.go).
 */
struct MANTLEEDITOR_API FMantleBodyOffsetShape
{
	/** weight+ weight- muscle+ muscle- chest+ chest- hips+ hips-. */
	FString Name;

	/** One array per LOD, LOD0 first: a position delta per vertex of that LOD, in that LOD's vertex order. */
	TArray<TArray<FVector3f>> LODs;

	/** A delta of each skin joint's LOCAL translation, in skin.joints order. */
	TArray<FVector3f> Joints;
};

/**
 * Builds body-shape offsets from two evaluations of MetaHuman Creator's body state (the normal one and one with a
 * shape's constraints set). The converter's built LOD0 is an exact vertex subset of the state's LOD1, up to the axis
 * map built = (state.x, state.z, state.y), so a shape is reshaped_state[map] - normal_state[map]. Pure except BuildForCharacter, which reads a MetaHuman Creator character.
 */
class MANTLEEDITOR_API FMantleBodyOffsetsBuilder
{
public:
	/** The state's axes in the built body's: (x, z, y). */
	static FVector3f StateToBuilt(const FVector3f& State) { return FVector3f(State.X, State.Z, State.Y); }

	/**
	 * For every built vertex, the index of the state vertex at the same position after the axis map. Fails when a
	 * built vertex has no state vertex within TolCm, or when two state vertices at different positions both match
	 * it (two at the exact same position are one: the lowest index wins).
	 */
	static bool LearnVertexMap(TConstArrayView<FVector3f> Built, TConstArrayView<FVector3f> StateLod,
		TArray<int32>& OutMap, FString& OutError, float TolCm = 0.001f);

	/** The shape's move of each built vertex, ShapeState[Map] - NormalState[Map], in built axes (cm). */
	static TArray<FVector3f> PositionDeltas(TConstArrayView<FVector3f> NormalState, TConstArrayView<FVector3f> ShapeState,
		TConstArrayView<int32> Map);

	/**
	 * Keeps the feet on the floor. The shape's change of the body's lowest point (min Z of NormalLod0 plus
	 * LodDeltas[0], minus min Z of NormalLod0) is subtracted as a constant Z from every vertex delta of every LOD
	 * and from the LOCAL translation delta of each root joint only (Parent outside the skin: INDEX_NONE). The
	 * children inherit the root's move through the hierarchy, so they are left alone: subtracting there too would
	 * move them twice. A root's local translation is taken to be in component axes (its parent is the component).
	 * Returns the Z removed (cm); the lowest point of LOD0 is then where it was, to float precision.
	 */
	static float StripVerticalOffset(TConstArrayView<FVector3f> NormalLod0, TArray<TArray<FVector3f>>& LodDeltas,
		TConstArrayView<int32> Parents, TArray<FVector3f>& LocalJointDeltas);

	/** Component-space rotations from the local ones; parents must come before their children. */
	static bool ComposeComponentRotations(TConstArrayView<int32> Parents, TConstArrayView<FQuat4f> LocalRotations,
		TArray<FQuat4f>& OutComponentRotations, FString& OutError);

	/**
	 * Component-space joint position deltas as deltas of each joint's LOCAL translation, in the built reference
	 * pose: localDelta(child) = parentRotation^-1 * (delta(child) - delta(parent)); a root uses its delta as it is.
	 * Joint scale is taken as 1 (the reference pose is unscaled) and rotations as unchanged by the shape.
	 */
	static bool LocalJointDeltas(TConstArrayView<int32> Parents, TConstArrayView<FQuat4f> ComponentRotations,
		TConstArrayView<FVector3f> ComponentDeltas, TArray<FVector3f>& OutLocalDeltas, FString& OutError);

	/**
	 * Like LearnVertexMap, for a state whose vertices only come near the built ones (the face: Creator refits its neck to
	 * the body, and the built face LOD0 is exact on ~91% of its vertices and within 1.5 mm on the rest). Each built vertex
	 * takes the NEAREST state vertex (the lowest index on a tie) provided it is within TolCm; ambiguity is not an error,
	 * because neighbouring vertices of a smooth surface move together. Fails, naming the vertex, when one has none in
	 * reach. OutExact counts the vertices matched to within 0.001 cm and OutWorstCm is the largest distance used.
	 */
	static bool LearnNearestVertexMap(TConstArrayView<FVector3f> Built, TConstArrayView<FVector3f> StateAll,
		TArray<int32>& OutMap, FString& OutError, float TolCm, int32* OutExact = nullptr, float* OutWorstCm = nullptr);

	/**
	 * Moves every vertex delta of every LOD, and the LOCAL translation delta of each root joint, down by Shift (cm) in Z:
	 * the second half of StripVerticalOffset, so the face can take the body's shift and the neck stays matched.
	 */
	static void ShiftVertical(float Shift, TArray<TArray<FVector3f>>& LodDeltas, TConstArrayView<int32> Parents,
		TArray<FVector3f>& LocalJointDeltas);

	/**
	 * Component-space delta of each joint of an exported skin, from deltas known by bone NAME. A joint named in Named takes
	 * its delta; any other (a twist or helper bone, a face-only bone, an ik bone) follows its parent rigidly, so its LOCAL
	 * delta comes out zero, and a root nobody named stays still. Parents must come before their children. Returns how many
	 * joints took a delta from Named, or INDEX_NONE when a parent does not come first.
	 */
	static int32 ComponentDeltasByName(TConstArrayView<FName> JointNames, TConstArrayView<int32> Parents,
		const TMap<FName, FVector3f>& Named, TArray<FVector3f>& OutComponentDeltas);

	/** Component-space joint positions from the local rest transforms (child = Local * parent); parents first. */
	static bool ComposeComponentPositions(TConstArrayView<int32> Parents, TConstArrayView<FTransform> Locals,
		TArray<FVector3f>& OutComponentPositions, FString& OutError);

	/** The Creator body constraint a shape moves: weight to "Fat", muscle to "Muscularity", chest to "Chest", hips to "Hip". NAME_None for any other name. */
	static FName ShapeConstraint(const FString& ShapeName);

	/**
	 * The measurement a shape sets its constraint to. weight+/- and muscle+/- go to the constraint's Max / Min; chest+/-
	 * and hips+/- to the character's Current measurement plus or minus 24 / 20 cm. Always clamped to [Min, Max].
	 */
	static float ShapeTargetMeasurement(const FString& ShapeName, float Current, float Min, float Max);

	/** One exported mesh (the body or the face) as BuildForCharacter reads it. Positions are borrowed, the skin is copied. */
	struct FTarget
	{
		/** Positions per LOD in the order they are written, LOD0 first: Unreal axes, cm. */
		TArray<TConstArrayView<FVector3f>> LODs;

		/** The skin, parent-first: bone names, parents (INDEX_NONE for a root) and local rest transforms. */
		TArray<FName> JointNames;
		TArray<int32> Parents;
		TArray<FTransform> Locals;
	};

	enum class EBuildResult : uint8
	{
		/** Shapes were made. */
		Built,
		/** No shapes, on purpose (a fixed body type has none to offer); OutMessage says why. */
		Skipped,
		/** No shapes, because something did not hold; OutMessage names it. */
		Failed,
	};

	/**
	 * The MetaHuman Creator character a converted body and face were built from, found from the body's build stamp
	 * (FMantleAvatarConverter::BuiltKey = <character folder>/Unpacked/<character name>). Null with OutError when the
	 * stamp is missing or the character is no longer there.
	 */
	static UMetaHumanCharacter* FindCharacterForBuild(const USkeletalMesh* BuiltBody, FString& OutError);

	/**
	 * The eight body shapes of Character on the converter's built Body and Face, in seconds (no build, no commit): each is
	 * evaluated on a COPY of the Creator body state with the preset's saved measurements released except Height, the
	 * shape's one constraint set, and the face refitted to that body the way Creator does on commit
	 * (UMetaHumanCharacterEditorSubsystem::UpdateFaceFromBodyInternal). The Z that lifts or sinks the feet is stripped from
	 * both. The character is opened for editing only if it is not already, and nothing on it is changed or saved.
	 * Face may be null (no face shapes then). Logs one MANTLE_CONVERT_OFFSETS line per shape.
	 */
	static EBuildResult BuildForCharacter(UMetaHumanCharacter* Character, const USkeletalMesh* BuiltBody, const FTarget& Body,
		const FTarget* Face, TArray<FMantleBodyOffsetShape>& OutBody, TArray<FMantleBodyOffsetShape>& OutFace, FString& OutMessage);
};
