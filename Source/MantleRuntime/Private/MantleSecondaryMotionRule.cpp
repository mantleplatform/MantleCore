// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSecondaryMotionRule.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSecondaryMotionRulePrivate
{
	void AddSecondaryChain(TArray<FMantleSecondaryChainDef>& Out, const TCHAR* Id, const TCHAR* BonePrefix, const int32 Count,
		const EMantleSecondaryPreset Preset)
	{
		FMantleSecondaryChainDef& Chain = Out.AddDefaulted_GetRef();
		Chain.Id = Id;
		Chain.DefaultPreset = Preset;
		for (int32 Index = 1; Index <= Count; ++Index)
		{
			Chain.Bones.Add(FName(*FString::Printf(TEXT("%s%02d"), BonePrefix, Index)));
		}
	}

	/** limits.json contentLimits.avatar_body.secondaryMotion.chains, in its order. */
	TArray<FMantleSecondaryChainDef> MakeSecondaryChains()
	{
		TArray<FMantleSecondaryChainDef> Out;
		for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
		{
			FMantleSecondaryChainDef& Chest = Out.AddDefaulted_GetRef();
			Chest.Id = FString::Printf(TEXT("chest_%s"), Side);
			Chest.Bones.Add(FName(*FString::Printf(TEXT("dyn_chest_%s"), Side)));
			Chest.DefaultPreset = EMantleSecondaryPreset::Chest;
		}
		for (const TCHAR* Letter : { TEXT("a"), TEXT("b"), TEXT("c"), TEXT("d"), TEXT("e"), TEXT("f") })
		{
			AddSecondaryChain(Out, *FString::Printf(TEXT("hair_%s"), Letter), *FString::Printf(TEXT("dyn_hair_%s_"), Letter), 4,
				EMantleSecondaryPreset::Hair);
		}
		AddSecondaryChain(Out, TEXT("back"), TEXT("dyn_back_"), 4, EMantleSecondaryPreset::Cloth);
		for (const TCHAR* Letter : { TEXT("a"), TEXT("b"), TEXT("c"), TEXT("d") })
		{
			AddSecondaryChain(Out, *FString::Printf(TEXT("cloth_%s"), Letter), *FString::Printf(TEXT("dyn_cloth_%s_"), Letter), 3,
				EMantleSecondaryPreset::Cloth);
		}
		AddSecondaryChain(Out, TEXT("tail"), TEXT("dyn_tail_"), 4, EMantleSecondaryPreset::Tail);
		return Out;
	}

	const TArray<FMantleSecondaryChainDef>& SecondaryChainTable()
	{
		static const TArray<FMantleSecondaryChainDef> Table = MakeSecondaryChains();
		return Table;
	}
}

TConstArrayView<FMantleSecondaryChainDef> MantleSecondaryMotion::Chains()
{
	return MantleSecondaryMotionRulePrivate::SecondaryChainTable();
}

const FMantleSecondaryChainDef* MantleSecondaryMotion::FindChain(const FString& Id)
{
	for (const FMantleSecondaryChainDef& Chain : Chains())
	{
		if (Chain.Id.Equals(Id, ESearchCase::CaseSensitive))
		{
			return &Chain;
		}
	}
	return nullptr;
}

const FMantleSecondaryChainDef* MantleSecondaryMotion::FindChainOfBone(const FName Bone, int32* OutIndex)
{
	for (const FMantleSecondaryChainDef& Chain : Chains())
	{
		const int32 Index = Chain.Bones.IndexOfByKey(Bone);
		if (Index != INDEX_NONE)
		{
			if (OutIndex)
			{
				*OutIndex = Index;
			}
			return &Chain;
		}
	}
	return nullptr;
}

bool MantleSecondaryMotion::IsSlotBone(const FName Bone)
{
	return !Bone.IsNone() && FindChainOfBone(Bone) != nullptr;
}

const TCHAR* MantleSecondaryMotion::PresetId(const EMantleSecondaryPreset Preset)
{
	switch (Preset)
	{
	case EMantleSecondaryPreset::Chest:
		return TEXT("chest");
	case EMantleSecondaryPreset::Cloth:
		return TEXT("cloth");
	case EMantleSecondaryPreset::Tail:
		return TEXT("tail");
	case EMantleSecondaryPreset::Hair:
	default:
		return TEXT("hair");
	}
}

bool MantleSecondaryMotion::PresetFromId(const FString& Id, EMantleSecondaryPreset& Out)
{
	for (const EMantleSecondaryPreset Preset :
		{ EMantleSecondaryPreset::Hair, EMantleSecondaryPreset::Chest, EMantleSecondaryPreset::Cloth, EMantleSecondaryPreset::Tail })
	{
		if (Id.Equals(PresetId(Preset), ESearchCase::CaseSensitive))
		{
			Out = Preset;
			return true;
		}
	}
	return false;
}

FText MantleSecondaryMotion::PresetDisplayName(const EMantleSecondaryPreset Preset)
{
	switch (Preset)
	{
	case EMantleSecondaryPreset::Chest:
		return NSLOCTEXT("MantleSecondaryMotion", "PresetChest", "Chest");
	case EMantleSecondaryPreset::Cloth:
		return NSLOCTEXT("MantleSecondaryMotion", "PresetCloth", "Cloth");
	case EMantleSecondaryPreset::Tail:
		return NSLOCTEXT("MantleSecondaryMotion", "PresetTail", "Tail");
	case EMantleSecondaryPreset::Hair:
	default:
		return NSLOCTEXT("MantleSecondaryMotion", "PresetHair", "Hair");
	}
}

FMantleSecondaryPresetParams MantleSecondaryMotion::PresetParams(const EMantleSecondaryPreset Preset)
{
	// limits.json secondaryMotion.presets. Natural frequencies of about 1.5 to 3 Hz, damping ratios near 0.4: a
	// swing that settles in about a second; the chest is stiffer and turns least. Gravity sags a 6 cm bone's tip by
	// Gravity / Stiffness cm at rest (a few degrees at most): the reference pose already hangs.
	FMantleSecondaryPresetParams Params;
	switch (Preset)
	{
	case EMantleSecondaryPreset::Chest:
		Params.Stiffness = 350.0f;
		Params.Damping = 14.0f;
		Params.Gravity = 30.0f;
		Params.MaxAngleDegrees = 12.0f;
		break;
	case EMantleSecondaryPreset::Cloth:
		Params.Stiffness = 160.0f;
		Params.Damping = 11.0f;
		Params.Gravity = 80.0f;
		Params.MaxAngleDegrees = 40.0f;
		break;
	case EMantleSecondaryPreset::Tail:
		Params.Stiffness = 90.0f;
		Params.Damping = 7.0f;
		Params.Gravity = 40.0f;
		Params.MaxAngleDegrees = 60.0f;
		break;
	case EMantleSecondaryPreset::Hair:
	default:
		Params.Stiffness = 120.0f;
		Params.Damping = 9.0f;
		Params.Gravity = 60.0f;
		Params.MaxAngleDegrees = 50.0f;
		break;
	}
	return Params;
}
