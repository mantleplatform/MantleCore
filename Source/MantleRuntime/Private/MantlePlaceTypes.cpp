// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceTypes.h"

namespace
{
	template <typename TEnum, int32 N>
	bool ParseEnum(const FString& In, const TCHAR* const (&Names)[N], TEnum& Out)
	{
		for (int32 i = 0; i < N; ++i)
		{
			if (In.Equals(Names[i], ESearchCase::CaseSensitive))
			{
				Out = static_cast<TEnum>(i);
				return true;
			}
		}
		return false;
	}

	const TCHAR* const GEntityTypeNames[] = {
		TEXT("StaticMesh"),
		TEXT("Group"),
		TEXT("PlayerStart"),
		TEXT("PointLight"),
		TEXT("SpotLight"),
		TEXT("DirectionalLight"),
		TEXT("TriggerVolume"),
		TEXT("SoundEmitter"),
		TEXT("InstancedMesh"),
		TEXT("CrowdSpawner"),
		TEXT("RectLight"),
		TEXT("Decal"),
		TEXT("SkeletalMesh"),
		TEXT("Terrain"),
		TEXT("VehicleSpawn"),
		TEXT("Effect"),
		TEXT("Constraint"),
		TEXT("Scatter"),
		TEXT("ReflectionProbe"),
		TEXT("WeaponSpawn"),
		TEXT("ScriptPoint"),
		TEXT("Text"),
		TEXT("WaterBody")
	};
	static_assert(UE_ARRAY_COUNT(GEntityTypeNames) == static_cast<SIZE_T>(EMantleEntityType::WaterBody) + 1, "one name per EMantleEntityType");

	// In EMantleWaterBodyType and EMantleWaterSplinePointType order; a WaterBody's type and its spline points' interp (#997).
	const TCHAR* const GWaterBodyTypeNames[] = {
		TEXT("lake"),
		TEXT("river"),
		TEXT("ocean"),
		TEXT("custom")
	};
	// In EMantleSeatStyle order; a seat's style (#142).
	const TCHAR* const GSeatStyleNames[] = {
		TEXT("bench")
	};
	const TCHAR* const GWaterSplinePointTypeNames[] = {
		TEXT("curve"),
		TEXT("linear"),
		TEXT("constant"),
		TEXT("curveClamped"),
		TEXT("curveCustomTangent")
	};

	// In EMantleTextHorizontalAlignment and EMantleTextVerticalAlignment order; a Text entity's alignments (#724).
	const TCHAR* const GTextHorizontalAlignmentNames[] = {
		TEXT("left"),
		TEXT("center"),
		TEXT("right")
	};
	const TCHAR* const GTextVerticalAlignmentNames[] = {
		TEXT("top"),
		TEXT("center"),
		TEXT("bottom"),
		TEXT("baseline")
	};

	// In EMantleReflectionProbeShape order; a ReflectionProbe's shape (#628).
	const TCHAR* const GReflectionProbeShapeNames[] = {
		TEXT("box"),
		TEXT("sphere")
	};

	// In EMantleConstraintType order; a Constraint's type (#528).
	const TCHAR* const GConstraintTypeNames[] = {
		TEXT("weld"),
		TEXT("hinge"),
		TEXT("rope")
	};

	const TCHAR* const GAssetKindNames[] = {
		TEXT("StaticMesh"),
		TEXT("Material"),
		TEXT("Texture"),
		TEXT("Sound"),
		TEXT("SkeletalMesh"),
		TEXT("AnimClip"),
		TEXT("Heightfield"),
		TEXT("Vehicle"),
		TEXT("Weapon")
	};

	const TCHAR* const GCollisionNames[] = {
		TEXT("none"),
		TEXT("blocking"),
		TEXT("overlap")
	};

	const TCHAR* const GTriggerShapeNames[] = {
		TEXT("box"),
		TEXT("sphere")
	};

	const TCHAR* const GRevisionStateNames[] = {
		TEXT("draft"),
		TEXT("live"),
		TEXT("archived")
	};

	const TCHAR* const GScriptSideNames[] = {
		TEXT("server"),
		TEXT("client"),
		TEXT("shared")
	};

	const TCHAR* const GScriptLanguageNames[] = {
		TEXT("luau"),
		TEXT("javascript"),
		TEXT("typescript"),
		TEXT("python")
	};

	// In EMantleMaturity / EMantlePlatform order; schema/place.schema.json's audience block.
	const TCHAR* const GMaturityNames[] = {
		TEXT("unrated"),
		TEXT("everyone"),
		TEXT("teen"),
		TEXT("mature")
	};

	// In EMantleLightUnits order, which is ELightUnits order; a light's intensityUnits (#385).
	const TCHAR* const GLightUnitsNames[] = {
		TEXT("unitless"),
		TEXT("candelas"),
		TEXT("lumens"),
		TEXT("ev"),
		TEXT("nits")
	};

	const TCHAR* const GPlatformNames[] = {
		TEXT("pc"),
		TEXT("playstation"),
		TEXT("xbox"),
		TEXT("switch"),
		TEXT("ios"),
		TEXT("android")
	};

	const TCHAR* const GPhysicsMaterialNames[] = {
		TEXT("default"),
		TEXT("rubber"),
		TEXT("ice"),
		TEXT("metal"),
		TEXT("wood"),
		TEXT("stone")
	};
	static_assert(static_cast<int32>(UE_ARRAY_COUNT(GPhysicsMaterialNames)) == MantlePhysicsMaterialCount,"one name per EMantlePhysicsMaterial");
}

const TCHAR* MantleEntityTypeToString(EMantleEntityType Type)
{
	const int32 Index = static_cast<int32>(Type);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GEntityTypeNames)) ? GEntityTypeNames[Index] : TEXT("Unknown");
}

bool MantleLooksLikePlaceId(const FString& Candidate)
{
	// A uuid: 8-4-4-4-12 hexadecimal. The server answers "not found" for anything else, so
	// this only exists to fail a typo instantly instead of after a round trip, and to keep
	// a pasted URL or file path from being sent as a path segment.
	const FString Trimmed = Candidate.TrimStartAndEnd();
	if (Trimmed.Len() != 36)
	{
		return false;
	}

	for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
	{
		const TCHAR Character = Trimmed[Index];
		const bool bShouldBeDash = Index == 8 || Index == 13 || Index == 18 || Index == 23;

		if (bShouldBeDash)
		{
			if (Character != TEXT('-'))
			{
				return false;
			}
		}
		else if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}

	return true;
}

bool MantleParseEntityType(const FString& In, EMantleEntityType& Out)
{
	return ParseEnum(In, GEntityTypeNames, Out);
}

bool MantleParseAssetKind(const FString& In, EMantleAssetKind& Out)
{
	return ParseEnum(In, GAssetKindNames, Out);
}

bool MantleParseCollision(const FString& In, EMantleCollision& Out)
{
	return ParseEnum(In, GCollisionNames, Out);
}

bool MantleParseTriggerShape(const FString& In, EMantleTriggerShape& Out)
{
	return ParseEnum(In, GTriggerShapeNames, Out);
}

bool MantleParseRevisionState(const FString& In, EMantleRevisionState& Out)
{
	return ParseEnum(In, GRevisionStateNames, Out);
}

const TCHAR* MantleScriptSideToString(EMantleScriptSide Side)
{
	const int32 Index = static_cast<int32>(Side);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GScriptSideNames)) ? GScriptSideNames[Index] : TEXT("server");
}

bool MantleParseScriptSide(const FString& In, EMantleScriptSide& Out)
{
	return ParseEnum(In, GScriptSideNames, Out);
}

const TCHAR* MantleScriptLanguageToString(EMantleScriptLanguage Language)
{
	const int32 Index = static_cast<int32>(Language);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GScriptLanguageNames))
		? GScriptLanguageNames[Index]
		: TEXT("luau");
}

bool MantleParseScriptLanguage(const FString& In, EMantleScriptLanguage& Out)
{
	return ParseEnum(In, GScriptLanguageNames, Out);
}

const TCHAR* MantleMaturityToString(EMantleMaturity Maturity)
{
	const int32 Index = static_cast<int32>(Maturity);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GMaturityNames)) ? GMaturityNames[Index] : TEXT("unrated");
}

bool MantleParseMaturity(const FString& In, EMantleMaturity& Out)
{
	return ParseEnum(In, GMaturityNames, Out);
}

const TCHAR* MantleLightUnitsToString(EMantleLightUnits Units)
{
	const int32 Index = static_cast<int32>(Units);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GLightUnitsNames)) ? GLightUnitsNames[Index] : TEXT("unitless");
}

bool MantleParseLightUnits(const FString& In, EMantleLightUnits& Out)
{
	return ParseEnum(In, GLightUnitsNames, Out);
}

const TCHAR* MantlePlatformToString(EMantlePlatform Platform)
{
	const int32 Index = static_cast<int32>(Platform);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GPlatformNames)) ? GPlatformNames[Index] : TEXT("pc");
}

bool MantleParsePlatform(const FString& In, EMantlePlatform& Out)
{
	return ParseEnum(In, GPlatformNames, Out);
}

const TCHAR* MantlePhysicsMaterialToString(EMantlePhysicsMaterial Material)
{
	const int32 Index = static_cast<int32>(Material);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GPhysicsMaterialNames)) ? GPhysicsMaterialNames[Index] : TEXT("default");
}

bool MantleParsePhysicsMaterial(const FString& In, EMantlePhysicsMaterial& Out)
{
	return ParseEnum(In, GPhysicsMaterialNames, Out);
}

const TCHAR* MantleConstraintTypeToString(EMantleConstraintType Type)
{
	const int32 Index = static_cast<int32>(Type);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GConstraintTypeNames)) ? GConstraintTypeNames[Index] : TEXT("weld");
}

bool MantleParseConstraintType(const FString& In, EMantleConstraintType& Out)
{
	return ParseEnum(In, GConstraintTypeNames, Out);
}

const TCHAR* MantleReflectionProbeShapeToString(EMantleReflectionProbeShape Shape)
{
	const int32 Index = static_cast<int32>(Shape);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GReflectionProbeShapeNames)) ? GReflectionProbeShapeNames[Index] : TEXT("box");
}

bool MantleParseReflectionProbeShape(const FString& In, EMantleReflectionProbeShape& Out)
{
	return ParseEnum(In, GReflectionProbeShapeNames, Out);
}

const TCHAR* MantleTextHorizontalAlignmentToString(EMantleTextHorizontalAlignment Alignment)
{
	const int32 Index = static_cast<int32>(Alignment);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GTextHorizontalAlignmentNames)) ? GTextHorizontalAlignmentNames[Index] : TEXT("left");
}

bool MantleParseTextHorizontalAlignment(const FString& In, EMantleTextHorizontalAlignment& Out)
{
	return ParseEnum(In, GTextHorizontalAlignmentNames, Out);
}

const TCHAR* MantleTextVerticalAlignmentToString(EMantleTextVerticalAlignment Alignment)
{
	const int32 Index = static_cast<int32>(Alignment);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GTextVerticalAlignmentNames)) ? GTextVerticalAlignmentNames[Index] : TEXT("top");
}

bool MantleParseTextVerticalAlignment(const FString& In, EMantleTextVerticalAlignment& Out)
{
	return ParseEnum(In, GTextVerticalAlignmentNames, Out);
}

const TCHAR* MantleWaterBodyTypeToString(EMantleWaterBodyType Type)
{
	const int32 Index = static_cast<int32>(Type);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GWaterBodyTypeNames)) ? GWaterBodyTypeNames[Index] : TEXT("lake");
}

bool MantleParseWaterBodyType(const FString& In, EMantleWaterBodyType& Out)
{
	return ParseEnum(In, GWaterBodyTypeNames, Out);
}

const TCHAR* MantleSeatStyleToString(EMantleSeatStyle Style)
{
	const int32 Index = static_cast<int32>(Style);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GSeatStyleNames)) ? GSeatStyleNames[Index] : TEXT("bench");
}

bool MantleParseSeatStyle(const FString& In, EMantleSeatStyle& Out)
{
	return ParseEnum(In, GSeatStyleNames, Out);
}

const TCHAR* MantleWaterSplinePointTypeToString(EMantleWaterSplinePointType Type)
{
	const int32 Index = static_cast<int32>(Type);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(GWaterSplinePointTypeNames)) ? GWaterSplinePointTypeNames[Index] : TEXT("curve");
}

bool MantleParseWaterSplinePointType(const FString& In, EMantleWaterSplinePointType& Out)
{
	return ParseEnum(In, GWaterSplinePointTypeNames, Out);
}
