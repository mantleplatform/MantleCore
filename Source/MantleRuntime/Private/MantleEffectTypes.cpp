// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEffectTypes.h"

#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"

#include "Misc/PackageName.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleEffectTypesPrivate
{
	const TCHAR* const ArchetypeNames[FMantleEffects::NumArchetypes] = {
		TEXT("smoke"), TEXT("fire"), TEXT("sparks"), TEXT("dust"), TEXT("glow"), TEXT("sprites")
	};
	const TCHAR* const ArchetypeDisplayNames[FMantleEffects::NumArchetypes] = {
		TEXT("Smoke"), TEXT("Fire"), TEXT("Sparks"), TEXT("Dust"), TEXT("Glow"), TEXT("Sprites")
	};

	/** Per archetype: looked for once, and found missing. Game thread. */
	bool GSystemChecked[FMantleEffects::NumArchetypes] = { false, false, false, false, false, false };
	bool GSystemMissing[FMantleEffects::NumArchetypes] = { false, false, false, false, false, false };
	bool GComponentClassChecked = false;
	UClass* GComponentClass = nullptr;

	int32 EffectArchetypeIndex(const EMantleEffectArchetype Archetype)
	{
		const int32 Index = static_cast<int32>(Archetype);
		return Index >= 0 && Index < FMantleEffects::NumArchetypes ? Index : 0;
	}

	FString EffectFolder()
	{
		return FMantleEffects::Folder;
	}

	/** Value clamped to Min..Max, or Fallback when it is not a finite number. */
	double ClampEffectNumber(const double Value, const double Min, const double Max, const double Fallback)
	{
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback;
	}

	float ClampEffectChannel(const float Value, const float Fallback)
	{
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0f, 1.0f) : Fallback;
	}
}

const TCHAR* FMantleEffects::ArchetypeName(const EMantleEffectArchetype Archetype)
{
	using namespace MantleEffectTypesPrivate;
	return ArchetypeNames[EffectArchetypeIndex(Archetype)];
}

const TCHAR* FMantleEffects::ArchetypeDisplayName(const EMantleEffectArchetype Archetype)
{
	using namespace MantleEffectTypesPrivate;
	return ArchetypeDisplayNames[EffectArchetypeIndex(Archetype)];
}

bool FMantleEffects::ParseArchetype(const FString& Name, EMantleEffectArchetype& Out)
{
	using namespace MantleEffectTypesPrivate;
	for (int32 Index = 0; Index < NumArchetypes; ++Index)
	{
		if (Name.Equals(ArchetypeNames[Index], ESearchCase::CaseSensitive))
		{
			Out = static_cast<EMantleEffectArchetype>(Index);
			return true;
		}
	}
	return false;
}

bool FMantleEffects::ParseArchetypeAnyCase(const FString& Name, EMantleEffectArchetype& Out)
{
	using namespace MantleEffectTypesPrivate;
	for (int32 Index = 0; Index < NumLookArchetypes; ++Index)
	{
		if (Name.Equals(ArchetypeNames[Index], ESearchCase::IgnoreCase))
		{
			Out = static_cast<EMantleEffectArchetype>(Index);
			return true;
		}
	}
	return false;
}

FString FMantleEffects::ArchetypeList()
{
	using namespace MantleEffectTypesPrivate;
	FString List;
	for (int32 Index = 0; Index < NumArchetypes; ++Index)
	{
		if (Index > 0)
		{
			List += Index == NumArchetypes - 1 ? TEXT(" or ") : TEXT(", ");
		}
		List += ArchetypeNames[Index];
	}
	return List;
}

FMantleEffectLook FMantleEffects::DefaultLook(const EMantleEffectArchetype Archetype)
{
	// Starting points for the owner's visual check (DOC-CHANGES.md); each archetype's system is tuned to
	// look right at these values, which is also what tools/effects/Create-MantleEffects.py sets as the
	// user parameters' defaults.
	FMantleEffectLook Look;
	switch (Archetype)
	{
	case EMantleEffectArchetype::Smoke:
		Look.Color = FLinearColor(0.35f, 0.35f, 0.35f);
		Look.Size = 120.0;
		Look.Rate = 12.0;
		Look.Speed = 60.0;
		Look.Lifetime = 4.0;
		break;
	case EMantleEffectArchetype::Fire:
		// NS_MantleFire (#1028, from Vefects' Free Fire) is its own fire at these values: white keeps its flames' own
		// colours (any other colour recolours them), and Size, Rate, Speed and Lifetime scale it from 40, 40, 120 and 1.
		Look.Color = FLinearColor::White;
		Look.Size = 40.0;
		Look.Rate = 40.0;
		Look.Speed = 120.0;
		Look.Lifetime = 1.0;
		break;
	case EMantleEffectArchetype::Sparks:
		Look.Color = FLinearColor(1.0f, 0.75f, 0.3f);
		Look.Size = 3.0;
		Look.Rate = 30.0;
		Look.Speed = 400.0;
		Look.Lifetime = 0.8;
		break;
	case EMantleEffectArchetype::Dust:
		Look.Color = FLinearColor(0.6f, 0.5f, 0.4f);
		Look.Size = 80.0;
		Look.Rate = 8.0;
		Look.Speed = 30.0;
		Look.Lifetime = 3.0;
		break;
	case EMantleEffectArchetype::Glow:
		Look.Color = FLinearColor(0.4f, 0.7f, 1.0f);
		Look.Size = 25.0;
		Look.Rate = 10.0;
		Look.Speed = 20.0;
		Look.Lifetime = 2.0;
		break;
	case EMantleEffectArchetype::Sprites:
		// Colour and rate only: a sprites effect's size, lifetime and motion are its FMantleSpritesSettings
		// (Size, Speed and Lifetime here are what an untouched sprites effect starts from, and are never written).
		Look.Color = FLinearColor::White;
		Look.Size = 25.0;
		Look.Rate = 10.0;
		Look.Speed = 0.0;
		Look.Lifetime = 2.0;
		break;
	}
	return Look;
}

const TCHAR* FMantleEffects::BlendName(const EMantleSpritesBlend Blend)
{
	return Blend == EMantleSpritesBlend::Additive ? TEXT("additive") : TEXT("translucent");
}

bool FMantleEffects::ParseBlend(const FString& Name, EMantleSpritesBlend& Out)
{
	if (Name.Equals(TEXT("translucent"), ESearchCase::CaseSensitive))
	{
		Out = EMantleSpritesBlend::Translucent;
		return true;
	}
	if (Name.Equals(TEXT("additive"), ESearchCase::CaseSensitive))
	{
		Out = EMantleSpritesBlend::Additive;
		return true;
	}
	return false;
}

const TCHAR* FMantleEffects::ShapeName(const EMantleSpritesShape Shape)
{
	switch (Shape)
	{
	case EMantleSpritesShape::Box: return TEXT("box");
	case EMantleSpritesShape::Sphere: return TEXT("sphere");
	case EMantleSpritesShape::Cylinder: return TEXT("cylinder");
	case EMantleSpritesShape::Point: break;
	}
	return TEXT("point");
}

bool FMantleEffects::ParseShape(const FString& Name, EMantleSpritesShape& Out)
{
	for (const EMantleSpritesShape Shape : { EMantleSpritesShape::Point, EMantleSpritesShape::Box, EMantleSpritesShape::Sphere, EMantleSpritesShape::Cylinder })
	{
		if (Name.Equals(ShapeName(Shape), ESearchCase::CaseSensitive))
		{
			Out = Shape;
			return true;
		}
	}
	return false;
}

const TCHAR* FMantleEffects::SubImageModeName(const EMantleSpritesSubImageMode Mode)
{
	return Mode == EMantleSpritesSubImageMode::OverLife ? TEXT("overLife") : TEXT("random");
}

bool FMantleEffects::ParseSubImageMode(const FString& Name, EMantleSpritesSubImageMode& Out)
{
	for (const EMantleSpritesSubImageMode Mode : { EMantleSpritesSubImageMode::Random, EMantleSpritesSubImageMode::OverLife })
	{
		if (Name.Equals(SubImageModeName(Mode), ESearchCase::CaseSensitive))
		{
			Out = Mode;
			return true;
		}
	}
	return false;
}

FMantleSpritesSettings FMantleEffects::ClampSprites(const FMantleSpritesSettings& In)
{
	using namespace MantleEffectTypesPrivate;
	const FMantleSpritesSettings Own;
	auto Axis = [](const double Value, const double Fallback)
	{
		return ClampEffectNumber(Value, -FMantlePlaceLimits::MaxSpriteVector, FMantlePlaceLimits::MaxSpriteVector, Fallback);
	};
	auto Length = [](const double Value, const double Fallback)
	{
		return ClampEffectNumber(Value, 0.0, FMantlePlaceLimits::MaxSpriteShapeLength, Fallback);
	};
	FMantleSpritesSettings Out = In;
	Out.ColorEnd = FLinearColor(
		ClampEffectChannel(In.ColorEnd.R, Own.ColorEnd.R), ClampEffectChannel(In.ColorEnd.G, Own.ColorEnd.G), ClampEffectChannel(In.ColorEnd.B, Own.ColorEnd.B));
	Out.Alpha = ClampEffectNumber(In.Alpha, 0.0, 1.0, Own.Alpha);
	Out.AlphaEnd = ClampEffectNumber(In.AlphaEnd, 0.0, 1.0, Own.AlphaEnd);
	Out.FadeIn = ClampEffectNumber(In.FadeIn, 0.0, 1.0, Own.FadeIn);
	Out.SizeMin = ClampEffectNumber(In.SizeMin, FMantlePlaceLimits::MinSpriteSize, FMantlePlaceLimits::MaxEffectSize, Own.SizeMin);
	Out.SizeMax = FMath::Max(Out.SizeMin, ClampEffectNumber(In.SizeMax, FMantlePlaceLimits::MinSpriteSize, FMantlePlaceLimits::MaxEffectSize, Own.SizeMax));
	Out.SizeEndScale = ClampEffectNumber(In.SizeEndScale, 0.0, FMantlePlaceLimits::MaxSpriteEndScale, Own.SizeEndScale);
	Out.LifetimeMin = ClampEffectNumber(In.LifetimeMin, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime, Own.LifetimeMin);
	Out.LifetimeMax = FMath::Max(Out.LifetimeMin,
		ClampEffectNumber(In.LifetimeMax, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime, Own.LifetimeMax));
	Out.VelocityMin = FVector(Axis(In.VelocityMin.X, 0.0), Axis(In.VelocityMin.Y, 0.0), Axis(In.VelocityMin.Z, 0.0));
	Out.VelocityMax = FVector(
		FMath::Max(Out.VelocityMin.X, Axis(In.VelocityMax.X, 0.0)), FMath::Max(Out.VelocityMin.Y, Axis(In.VelocityMax.Y, 0.0)),
		FMath::Max(Out.VelocityMin.Z, Axis(In.VelocityMax.Z, 0.0)));
	Out.Acceleration = FVector(Axis(In.Acceleration.X, 0.0), Axis(In.Acceleration.Y, 0.0), Axis(In.Acceleration.Z, 0.0));
	Out.BoxHalfExtent = FVector(
		Length(In.BoxHalfExtent.X, Own.BoxHalfExtent.X), Length(In.BoxHalfExtent.Y, Own.BoxHalfExtent.Y), Length(In.BoxHalfExtent.Z, Own.BoxHalfExtent.Z));
	Out.SphereRadius = Length(In.SphereRadius, Own.SphereRadius);
	Out.CylinderRadius = Length(In.CylinderRadius, Own.CylinderRadius);
	Out.CylinderHeight = Length(In.CylinderHeight, Own.CylinderHeight);
	Out.MaxParticles = FMath::Clamp(In.MaxParticles, 1, FMantlePlaceLimits::MaxSpriteParticles);
	Out.SubImageColumns = FMath::Clamp(In.SubImageColumns, 1, FMantlePlaceLimits::MaxSpriteSubImages);
	Out.SubImageRows = FMath::Clamp(In.SubImageRows, 1, FMantlePlaceLimits::MaxSpriteSubImages);
	return Out;
}

void FMantleEffects::ResolveSpritesPair(const bool bHasMin, const bool bHasMax, double& Min, double& Max)
{
	if (bHasMin && !bHasMax)
	{
		Max = Min;
	}
	else if (bHasMax && !bHasMin)
	{
		Min = Max;
	}
}

void FMantleEffects::ResolveSpritesVectorPair(const bool bHasMin, const bool bHasMax, FVector& Min, FVector& Max)
{
	if (bHasMin && !bHasMax)
	{
		Max = Min;
	}
	else if (bHasMax && !bHasMin)
	{
		Min = Max;
	}
}

double FMantleEffects::SpritesEffectiveRate(const double Rate, const FMantleSpritesSettings& Settings)
{
	const double Lifetime = FMath::Max(Settings.LifetimeMax, FMantlePlaceLimits::MinEffectLifetime);
	const double Allowed = static_cast<double>(FMath::Clamp(Settings.MaxParticles, 1, FMantlePlaceLimits::MaxSpriteParticles)) / Lifetime;
	return FMath::Min(Rate, Allowed);
}

FString FMantleEffects::SpritesMaterialPath(const EMantleSpritesBlend Blend)
{
	const FString Name = Blend == EMantleSpritesBlend::Additive ? TEXT("M_MantleSprite_Additive") : TEXT("M_MantleSprite_Translucent");
	return FString::Printf(TEXT("%s/%s.%s"), *MantleEffectTypesPrivate::EffectFolder(), *Name, *Name);
}

FMantleEffectLook FMantleEffects::ClampLook(const FMantleEffectLook& In, const EMantleEffectArchetype Archetype)
{
	using namespace MantleEffectTypesPrivate;
	const FMantleEffectLook Own = DefaultLook(Archetype);
	FMantleEffectLook Out;
	Out.Color = FLinearColor(
		ClampEffectChannel(In.Color.R, Own.Color.R),
		ClampEffectChannel(In.Color.G, Own.Color.G),
		ClampEffectChannel(In.Color.B, Own.Color.B));
	Out.Size = ClampEffectNumber(In.Size, FMantlePlaceLimits::MinEffectSize, FMantlePlaceLimits::MaxEffectSize, Own.Size);
	Out.Rate = ClampEffectNumber(In.Rate, 0.0, FMantlePlaceLimits::MaxEffectRate, Own.Rate);
	Out.Speed = ClampEffectNumber(In.Speed, 0.0, FMantlePlaceLimits::MaxEffectSpeed, Own.Speed);
	Out.Lifetime = ClampEffectNumber(In.Lifetime, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime, Own.Lifetime);
	return Out;
}

FString FMantleEffects::SystemPath(const EMantleEffectArchetype Archetype)
{
	const FString Name = FString::Printf(TEXT("NS_Mantle%s"), ArchetypeDisplayName(Archetype));
	return FString::Printf(TEXT("%s/%s.%s"), *MantleEffectTypesPrivate::EffectFolder(), *Name, *Name);
}

UFXSystemAsset* FMantleEffects::LoadSystem(const EMantleEffectArchetype Archetype)
{
	using namespace MantleEffectTypesPrivate;
	const int32 Index = EffectArchetypeIndex(Archetype);
	if (GSystemMissing[Index])
	{
		return nullptr;
	}
	const FSoftObjectPath Path(SystemPath(Archetype));
	UFXSystemAsset* Loaded = Cast<UFXSystemAsset>(Path.ResolveObject());
	if (!Loaded && (GSystemChecked[Index] || FPackageName::DoesPackageExist(Path.GetLongPackageName())))
	{
		// Synchronous, once per archetype per process: the first effect of it pays the load.
		Loaded = LoadObject<UFXSystemAsset>(nullptr, *Path.ToString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	GSystemChecked[Index] = true;
	if (!Loaded)
	{
		GSystemMissing[Index] = true;
		// A warning, not an error: the place is whole without its effects, and the log line is the handoff.
		UE_LOG(LogMantle, Warning,
			TEXT("MANTLE_PLACE_EFFECT %s is missing from this build (tools/effects/Create-MantleEffects.py makes it): %s effects draw nothing"),
			*Path.ToString(), ArchetypeName(Archetype));
	}
	return Loaded;
}

void FMantleEffects::WarmSystems()
{
	using namespace MantleEffectTypesPrivate;
	for (int32 Index = 0; Index < NumArchetypes; ++Index)
	{
		const EMantleEffectArchetype Archetype = static_cast<EMantleEffectArchetype>(Index);
		if (GSystemMissing[EffectArchetypeIndex(Archetype)])
		{
			continue;
		}
		const FSoftObjectPath Path(SystemPath(Archetype));
		if (!Path.ResolveObject() && FPackageName::DoesPackageExist(Path.GetLongPackageName()))
		{
			// Nothing to do when it lands: LoadSystem finds it already in memory.
			LoadPackageAsync(Path.GetLongPackageName());
		}
	}
}

UClass* FMantleEffects::ComponentClass()
{
	using namespace MantleEffectTypesPrivate;
	if (!GComponentClassChecked)
	{
		GComponentClassChecked = true;
		UClass* Found = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraComponent"));
		// The effect component is driven through UFXSystemComponent's interface only.
		GComponentClass = Found && Found->IsChildOf(UFXSystemComponent::StaticClass()) ? Found : nullptr;
		if (!GComponentClass)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_EFFECT the Niagara plugin is not loaded in this build: place effects draw nothing"));
		}
	}
	return GComponentClass;
}
