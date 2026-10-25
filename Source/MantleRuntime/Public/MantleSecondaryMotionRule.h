// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * GhzGarage/mantle#595: the standard optional dynamic bone slots an avatar body may carry for secondary motion, and the
 * presets its chains move with. The one copy of limits.json contentLimits.avatar_body.secondaryMotion in C++, shared by
 * the game (the spring solver on the visible body, FMantleSecondaryMotion) and the creator plugin (mapping a creator's
 * bone chains onto the slots).
 *
 * A chain is a fixed list of dyn_* bones, each under the one before; its first bone hangs from a bone of the skeleton
 * (the skeleton fixture says which). The slots are on the UE5, UE4 and UEFN Mannequin skeletons only (ue5mannequin, ue4mannequin, uefnmannequin).
 */
enum class EMantleSecondaryPreset : uint8
{
	Hair,
	Chest,
	Cloth,
	Tail,
};

/** One preset's spring: pulls each bone's tip toward where the animation puts it, relative to that target's own motion. */
struct FMantleSecondaryPresetParams
{
	/** Acceleration per cm of offset from the animated tip, 1/s^2. */
	float Stiffness = 0.0f;
	/** Acceleration per cm/s of the tip's velocity relative to its target's, 1/s. */
	float Damping = 0.0f;
	/** Downward acceleration on the tip, cm/s^2 (a small bias: the reference pose already hangs). */
	float Gravity = 0.0f;
	/** How far a bone may turn from its animated direction, degrees. */
	float MaxAngleDegrees = 0.0f;
};

/** One slot chain (limits.json secondaryMotion.chains entry). */
struct FMantleSecondaryChainDef
{
	/** The chain id an item's secondaryMotion names ("hair_a"), case-sensitive. */
	FString Id;
	/** Its slot bones, first to last. */
	TArray<FName> Bones;
	EMantleSecondaryPreset DefaultPreset = EMantleSecondaryPreset::Hair;
};

namespace MantleSecondaryMotion
{
	constexpr int32 NumPresets = 4;

	/** Every chain, in limits.json's order, which is also the order the runtime's bone budget keeps them in. */
	MANTLERUNTIME_API TConstArrayView<FMantleSecondaryChainDef> Chains();

	/** The chain with this id (case-sensitive), or null. */
	MANTLERUNTIME_API const FMantleSecondaryChainDef* FindChain(const FString& Id);

	/** The chain Bone is a slot of, or null; OutIndex is its place in the chain. */
	MANTLERUNTIME_API const FMantleSecondaryChainDef* FindChainOfBone(FName Bone, int32* OutIndex = nullptr);

	/** Whether Bone is one of the slots (any chain). */
	MANTLERUNTIME_API bool IsSlotBone(FName Bone);

	/** "hair", "chest", "cloth", "tail". */
	MANTLERUNTIME_API const TCHAR* PresetId(EMantleSecondaryPreset Preset);

	/** The preset with this id (case-sensitive): false for anything else, Out untouched. */
	MANTLERUNTIME_API bool PresetFromId(const FString& Id, EMantleSecondaryPreset& Out);

	/** Hair, Chest, Cloth, Tail: what the plugin's preset picker says. */
	MANTLERUNTIME_API FText PresetDisplayName(EMantleSecondaryPreset Preset);

	MANTLERUNTIME_API FMantleSecondaryPresetParams PresetParams(EMantleSecondaryPreset Preset);
}
