// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEffectExport.h"

#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantleEffectTypes.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceTypes.h"
#include "MantleTargetFeatures.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Distributions/DistributionFloatConstant.h"
#include "Distributions/DistributionFloatConstantCurve.h"
#include "Distributions/DistributionFloatUniform.h"
#include "Distributions/DistributionFloatUniformCurve.h"
#include "Distributions/DistributionVectorConstant.h"
#include "Distributions/DistributionVectorConstantCurve.h"
#include "Distributions/DistributionVectorUniform.h"
#include "Distributions/DistributionVectorUniformCurve.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Particles/Acceleration/ParticleModuleAcceleration.h"
#include "Particles/Acceleration/ParticleModuleAccelerationConstant.h"
#include "Particles/Color/ParticleModuleColor.h"
#include "Particles/Color/ParticleModuleColorOverLife.h"
#include "Particles/Lifetime/ParticleModuleLifetime.h"
#include "Particles/Location/ParticleModuleLocation.h"
#include "Particles/Location/ParticleModuleLocationPrimitiveCylinder.h"
#include "Particles/Location/ParticleModuleLocationPrimitiveSphere.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Particles/Size/ParticleModuleSize.h"
#include "Particles/Size/ParticleModuleSizeMultiplyLife.h"
#include "Particles/Size/ParticleModuleSizeScale.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/SubUV/ParticleModuleSubUV.h"
#include "Particles/SubUV/ParticleModuleSubUVMovie.h"
#include "Particles/SubUVAnimation.h"
#include "Particles/TypeData/ParticleModuleTypeDataBase.h"
#include "Particles/Velocity/ParticleModuleVelocity.h"
#include "Particles/Velocity/ParticleModuleVelocityCone.h"
#include "UObject/UObjectGlobals.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleEffectExportPrivate
{
	/** What one emitter's translation found: the effect it becomes, the texture it names, and what it only approximated. */
	struct FTranslatedEmitter
	{
		FMantleEffectProps Props;
		FString TexturePath;
		TArray<FString> Approximations;
		TArray<FString> Ignored;
	};

	/** Samples a float distribution at T (0..1 over life): its lowest and highest value there. False for one that is not a constant, range or curve. */
	bool SampleFloat(const UDistributionFloat* Distribution, const float T, double& OutMin, double& OutMax, bool& bOutCurve)
	{
		bOutCurve = false;
		if (const UDistributionFloatConstant* Constant = Cast<UDistributionFloatConstant>(Distribution))
		{
			OutMin = OutMax = Constant->Constant;
			return true;
		}
		if (const UDistributionFloatUniform* Uniform = Cast<UDistributionFloatUniform>(Distribution))
		{
			OutMin = FMath::Min(Uniform->Min, Uniform->Max);
			OutMax = FMath::Max(Uniform->Min, Uniform->Max);
			return true;
		}
		if (const UDistributionFloatConstantCurve* Curve = Cast<UDistributionFloatConstantCurve>(Distribution))
		{
			OutMin = OutMax = Curve->ConstantCurve.Eval(T);
			bOutCurve = Curve->ConstantCurve.Points.Num() > 1;
			return true;
		}
		if (const UDistributionFloatUniformCurve* Range = Cast<UDistributionFloatUniformCurve>(Distribution))
		{
			const FVector2D Value = Range->ConstantCurve.Eval(T);
			OutMin = FMath::Min(Value.X, Value.Y);
			OutMax = FMath::Max(Value.X, Value.Y);
			bOutCurve = Range->ConstantCurve.Points.Num() > 1;
			return true;
		}
		return false;
	}

	bool SampleVector(const UDistributionVector* Distribution, const float T, FVector& OutMin, FVector& OutMax, bool& bOutCurve)
	{
		bOutCurve = false;
		if (const UDistributionVectorConstant* Constant = Cast<UDistributionVectorConstant>(Distribution))
		{
			OutMin = OutMax = Constant->Constant;
			return true;
		}
		if (const UDistributionVectorUniform* Uniform = Cast<UDistributionVectorUniform>(Distribution))
		{
			OutMin = Uniform->Min.ComponentMin(Uniform->Max);
			OutMax = Uniform->Min.ComponentMax(Uniform->Max);
			return true;
		}
		if (const UDistributionVectorConstantCurve* Curve = Cast<UDistributionVectorConstantCurve>(Distribution))
		{
			OutMin = OutMax = Curve->ConstantCurve.Eval(T);
			bOutCurve = Curve->ConstantCurve.Points.Num() > 1;
			return true;
		}
		if (const UDistributionVectorUniformCurve* Range = Cast<UDistributionVectorUniformCurve>(Distribution))
		{
			const FTwoVectors Value = Range->ConstantCurve.Eval(T);
			OutMin = Value.v1.ComponentMin(Value.v2);
			OutMax = Value.v1.ComponentMax(Value.v2);
			bOutCurve = Range->ConstantCurve.Points.Num() > 1;
			return true;
		}
		return false;
	}

	double Average(const double A, const double B)
	{
		return 0.5 * (A + B);
	}

	/** A module's short name for a coverage line: "ParticleModuleRotationRate" is "RotationRate". */
	FString ModuleLabel(const UParticleModule* Module)
	{
		FString Name = Module->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("ParticleModule"));
		return Name;
	}

	/** True for a value that fits the field after clamping, so a clamp can be reported when it did not. */
	bool Nearly(const double A, const double B)
	{
		return FMath::IsNearlyEqual(A, B, 1.0e-6);
	}

	bool NearlyVector(const FVector& A, const FVector& B)
	{
		return Nearly(A.X, B.X) && Nearly(A.Y, B.Y) && Nearly(A.Z, B.Z);
	}

	/** The material's base texture and blend: additive stays additive, anything else is translucent. */
	void ReadMaterial(const UMaterialInterface* Material, FTranslatedEmitter& Out)
	{
		if (!Material)
		{
			Out.Approximations.Add(TEXT("no material, so a soft round dot"));
			return;
		}
		const EBlendMode Blend = Material->GetBlendMode();
		Out.Props.Sprites.Blend = Blend == BLEND_Additive ? EMantleSpritesBlend::Additive : EMantleSpritesBlend::Translucent;
		if (Blend != BLEND_Additive && Blend != BLEND_Translucent && Blend != BLEND_AlphaComposite)
		{
			Out.Approximations.Add(FString::Printf(TEXT("%s material drawn translucent"), *StaticEnum<EBlendMode>()->GetNameStringByValue(Blend)));
		}
		TArray<UTexture*> Used;
		Material->GetUsedTextures(Used);
		int32 Candidates = 0;
		for (UTexture* Texture : Used)
		{
			const UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (!Texture2D || Texture2D->CompressionSettings == TC_Normalmap)
			{
				continue;
			}
			if (++Candidates == 1)
			{
				Out.TexturePath = Texture2D->GetPathName();
			}
		}
		if (Candidates > 1)
		{
			Out.Approximations.Add(FString::Printf(TEXT("the first of the material's %d textures"), Candidates));
		}
		else if (Candidates == 0)
		{
			Out.Approximations.Add(TEXT("the material has no texture, so a soft round dot"));
		}
	}

	/**
	 * A flipbook sheet (#628): the Required module's SubImages_Horizontal by SubImages_Vertical (or the SubUV animation asset's),
	 * shown one frame per particle, which the SubUV module's interpolation method (a Required module setting) picks: the Random
	 * methods become subImageMode random, the Linear ones overLife. True when the sheet is carried; false (nothing set) when it
	 * is not: method None (Cascade then draws the whole texture), a 1 by 1 sheet, or one past Mantle's 16 by 16.
	 */
	bool ReadSubImages(const UParticleModuleRequired* Required, const UParticleModuleSubUV* SubUV, FMantleSpritesSettings& S, FTranslatedEmitter& Out)
	{
		if (!Required || !SubUV)
		{
			return false;
		}
		int32 Columns = Required->SubImages_Horizontal;
		int32 Rows = Required->SubImages_Vertical;
		if (const USubUVAnimation* Animation = SubUV->Animation.Get())
		{
			if (Animation->SubImages_Horizontal > 0 && Animation->SubImages_Vertical > 0)
			{
				Columns = Animation->SubImages_Horizontal;
				Rows = Animation->SubImages_Vertical;
			}
		}
		const EParticleSubUVInterpMethod Method = Required->InterpolationMethod.GetValue();
		if (Method == PSUVIM_None || (Columns <= 1 && Rows <= 1))
		{
			return false;
		}
		if (Columns > FMantlePlaceLimits::MaxSpriteSubImages || Rows > FMantlePlaceLimits::MaxSpriteSubImages)
		{
			Out.Approximations.Add(FString::Printf(TEXT("a %d by %d flipbook sheet is past Mantle's %d by %d, so the whole texture"),
				Columns, Rows, FMantlePlaceLimits::MaxSpriteSubImages, FMantlePlaceLimits::MaxSpriteSubImages));
			return false;
		}
		S.SubImageColumns = FMath::Max(Columns, 1);
		S.SubImageRows = FMath::Max(Rows, 1);
		const bool bBlend = Method == PSUVIM_Linear_Blend || Method == PSUVIM_Random_Blend;
		if (bBlend)
		{
			Out.Approximations.Add(TEXT("blending between flipbook frames dropped"));
		}
		if (SubUV->IsA<UParticleModuleSubUVMovie>())
		{
			S.SubImageMode = EMantleSpritesSubImageMode::OverLife;
			Out.Approximations.Add(TEXT("a SubUV movie's frame rate becomes the sheet played over a particle's life"));
		}
		else if (Method == PSUVIM_Random || Method == PSUVIM_Random_Blend)
		{
			S.SubImageMode = EMantleSpritesSubImageMode::Random;
			if (Required->RandomImageChanges != 0)
			{
				Out.Approximations.Add(TEXT("a random frame that changes during a life is held for the whole life"));
			}
		}
		else
		{
			// Linear: the SubImageIndex curve over a life. One that stays on a single frame is no animation: a random frame.
			double First = 0.0, Last = 0.0, Unused = 0.0;
			bool bCurve = false;
			const double Frames = static_cast<double>(Columns * Rows);
			if (SampleFloat(SubUV->SubImageIndex.Distribution, 0.0f, First, Unused, bCurve) && SampleFloat(SubUV->SubImageIndex.Distribution, 1.0f, Last, Unused, bCurve))
			{
				if (FMath::Abs(Last - First) < 0.5)
				{
					S.SubImageMode = EMantleSpritesSubImageMode::Random;
					Out.Approximations.Add(TEXT("a fixed flipbook frame becomes a random one"));
				}
				else
				{
					S.SubImageMode = EMantleSpritesSubImageMode::OverLife;
					if (First > 0.5 || Last < Frames - 1.5 || Last > Frames + 0.5)
					{
						Out.Approximations.Add(TEXT("the flipbook's frame range becomes the whole sheet over a life"));
					}
				}
			}
			else
			{
				S.SubImageMode = EMantleSpritesSubImageMode::OverLife;
			}
		}
		return true;
	}

	/**
	 * One Cascade emitter's LOD 0 onto sprites keys. False (with Dropped lines) for an emitter that cannot be carried: a
	 * mesh, beam, ribbon or trail emitter, one with attractors, or a disabled one. Reads each module the sprites keys have a
	 * place for; every other enabled module goes in Ignored.
	 */
	bool TranslateEmitter(UParticleEmitter* Emitter, const UParticleSystemComponent* Component, const FString& Name, FMantleCoverage& Coverage,
		FTranslatedEmitter& Out)
	{
		UParticleLODLevel* Lod = Emitter ? Emitter->GetLODLevel(0) : nullptr;
		if (!Lod || !Lod->bEnabled)
		{
			return false; // an emitter switched off draws nothing: nothing to report
		}
		if (const UParticleModule* TypeData = Lod->TypeDataModule)
		{
			const FString TypeName = TypeData->GetClass()->GetName();
			const TCHAR* Kind = nullptr;
			if (TypeName.Contains(TEXT("TypeDataMesh"))) { Kind = TEXT("mesh"); }
			else if (TypeName.Contains(TEXT("TypeDataBeam"))) { Kind = TEXT("beam"); }
			else if (TypeName.Contains(TEXT("TypeDataRibbon"))) { Kind = TEXT("ribbon"); }
			else if (TypeName.Contains(TEXT("TypeDataAnimTrail"))) { Kind = TEXT("anim trail"); }
			if (Kind)
			{
				Coverage.Add(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped, Name,
					FString::Printf(TEXT("a %s emitter: Mantle's sprites carry only sprite emitters"), Kind));
				return false;
			}
		}
		for (const UParticleModule* Module : Lod->Modules)
		{
			const FString ClassName = Module ? Module->GetClass()->GetName() : FString();
			if (Module && Module->bEnabled && (ClassName.Contains(TEXT("Attractor")) || ClassName.Contains(TEXT("VectorField"))))
			{
				Coverage.Add(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped, Name,
					FString::Printf(TEXT("an emitter with %s: attractors and vector fields are not carried"), *ModuleLabel(Module)));
				return false;
			}
		}

		FMantleEffectProps& Props = Out.Props;
		Props.Archetype = EMantleEffectArchetype::Sprites;
		Props.Look = FMantleEffects::DefaultLook(EMantleEffectArchetype::Sprites);
		Props.bAutoplay = Component->bAutoActivate;
		FMantleSpritesSettings& S = Props.Sprites;
		bool bCurveUsed = false;

		const UParticleModuleRequired* Required = Lod->RequiredModule;
		ReadMaterial(Required ? Required->Material.Get() : nullptr, Out);
		if (Required)
		{
			Props.bLoop = Required->EmitterLoops == 0;
			if (Props.bLoop == false)
			{
				Out.Approximations.Add(FString::Printf(TEXT("%d emitter loop(s) become one burst each time it starts"), Required->EmitterLoops));
			}
		}

		// Spawn: a steady rate (its mean), plus bursts spread over the emitter's duration.
		const float Duration = Required && Required->EmitterDuration > 0.0f ? Required->EmitterDuration : 1.0f;
		if (const UParticleModuleSpawn* Spawn = Lod->SpawnModule)
		{
			double RateMin = 0.0, RateMax = 0.0;
			bool bCurve = false;
			if (SampleFloat(Spawn->Rate.Distribution, 0.0f, RateMin, RateMax, bCurve))
			{
				Props.Look.Rate = Average(RateMin, RateMax);
				bCurveUsed = bCurveUsed || bCurve;
			}
			double Bursts = 0.0;
			for (const FParticleBurst& Burst : Spawn->BurstList)
			{
				Bursts += Burst.CountLow >= 0 ? 0.5 * (Burst.Count + Burst.CountLow) : Burst.Count;
			}
			if (Bursts > 0.0)
			{
				Props.Look.Rate += Bursts / Duration;
				Out.Approximations.Add(TEXT("bursts spread into a steady rate"));
			}
		}

		bool bLocation = false;
		bool bColorOverLife = false;
		bool bAlphaOverLife = false;
		bool bSizeStart = false;
		FVector Direction = FVector::ZeroVector;
		const UParticleModuleSubUV* SubUV = nullptr;
		for (UParticleModule* Module : Lod->Modules)
		{
			if (!Module || !Module->bEnabled)
			{
				continue;
			}
			// Read above, or (a sprite emitter's own type data) nothing to report.
			if (Module == Lod->RequiredModule.Get() || Module == Lod->SpawnModule.Get() || Module == Lod->TypeDataModule.Get())
			{
				continue;
			}
			bool bCurve = false;
			if (const UParticleModuleLifetime* Lifetime = Cast<UParticleModuleLifetime>(Module))
			{
				double Min = 0.0, Max = 0.0;
				if (SampleFloat(Lifetime->Lifetime.Distribution, 0.0f, Min, Max, bCurve))
				{
					S.LifetimeMin = Min;
					S.LifetimeMax = Max;
					bCurveUsed = bCurveUsed || bCurve;
				}
			}
			else if (const UParticleModuleSize* Size = Cast<UParticleModuleSize>(Module))
			{
				FVector Min, Max;
				if (SampleVector(Size->StartSize.Distribution, 0.0f, Min, Max, bCurve))
				{
					// A sprite is square: the mean of its width and height.
					S.SizeMin = Average(Min.X, Min.Y);
					S.SizeMax = Average(Max.X, Max.Y);
					bSizeStart = true;
					bCurveUsed = bCurveUsed || bCurve;
				}
			}
			else if (const UParticleModuleSizeScale* Scale = Cast<UParticleModuleSizeScale>(Module))
			{
				FVector Start, End, Unused;
				bool bEndCurve = false;
				if (SampleVector(Scale->SizeScale.Distribution, 0.0f, Start, Unused, bCurve) && SampleVector(Scale->SizeScale.Distribution, 1.0f, End, Unused, bEndCurve)
					&& Start.X > UE_KINDA_SMALL_NUMBER)
				{
					S.SizeEndScale = End.X / Start.X;
					Out.Approximations.Add(TEXT("size by life reduced to its start and end"));
				}
			}
			else if (const UParticleModuleSizeMultiplyLife* Multiply = Cast<UParticleModuleSizeMultiplyLife>(Module))
			{
				FVector Start, End, Unused;
				bool bEndCurve = false;
				if (SampleVector(Multiply->LifeMultiplier.Distribution, 0.0f, Start, Unused, bCurve) && SampleVector(Multiply->LifeMultiplier.Distribution, 1.0f, End, Unused, bEndCurve)
					&& Start.X > UE_KINDA_SMALL_NUMBER)
				{
					S.SizeEndScale = End.X / Start.X;
					Out.Approximations.Add(TEXT("size by life reduced to its start and end"));
				}
			}
			else if (const UParticleModuleVelocity* Velocity = Cast<UParticleModuleVelocity>(Module))
			{
				FVector Min, Max;
				if (SampleVector(Velocity->StartVelocity.Distribution, 0.0f, Min, Max, bCurve))
				{
					S.VelocityMin += Min;
					S.VelocityMax += Max;
				}
				double RadialMin = 0.0, RadialMax = 0.0;
				if (SampleFloat(Velocity->StartVelocityRadial.Distribution, 0.0f, RadialMin, RadialMax, bCurve) && (RadialMin != 0.0 || RadialMax != 0.0))
				{
					Out.Approximations.Add(TEXT("radial velocity not carried"));
				}
			}
			else if (const UParticleModuleVelocityCone* Cone = Cast<UParticleModuleVelocityCone>(Module))
			{
				double SpeedMin = 0.0, SpeedMax = 0.0, AngleMin = 0.0, AngleMax = 0.0;
				if (SampleFloat(Cone->Velocity.Distribution, 0.0f, SpeedMin, SpeedMax, bCurve)
					&& SampleFloat(Cone->Angle.Distribution, 0.0f, AngleMin, AngleMax, bCurve))
				{
					Direction = Cone->Direction.GetSafeNormal();
					if (Direction.IsNearlyZero())
					{
						Direction = FVector::UpVector;
					}
					const double Sin = FMath::Sin(FMath::DegreesToRadians(FMath::Clamp(AngleMax, 0.0, 180.0)));
					const double Cos = AngleMax >= 90.0 ? 0.0 : FMath::Cos(FMath::DegreesToRadians(AngleMax));
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						// Along the cone's axis from speed*cos(angle) to speed; across it the disc's extent on this axis.
						const double Along0 = Direction[Axis] * SpeedMin * Cos;
						const double Along1 = Direction[Axis] * SpeedMax;
						const double Spread = SpeedMax * Sin * FMath::Sqrt(FMath::Max(0.0, 1.0 - Direction[Axis] * Direction[Axis]));
						S.VelocityMin[Axis] += FMath::Min(Along0, Along1) - Spread;
						S.VelocityMax[Axis] += FMath::Max(Along0, Along1) + Spread;
					}
					if (AngleMax > 0.0)
					{
						Out.Approximations.Add(TEXT("a velocity cone becomes a box of velocities"));
					}
				}
			}
			else if (const UParticleModuleLocationPrimitiveCylinder* Cylinder = Cast<UParticleModuleLocationPrimitiveCylinder>(Module))
			{
				if (!bLocation)
				{
					double RadiusMin = 0.0, RadiusMax = 0.0, HeightMin = 0.0, HeightMax = 0.0;
					if (SampleFloat(Cylinder->StartRadius.Distribution, 0.0f, RadiusMin, RadiusMax, bCurve)
						&& SampleFloat(Cylinder->StartHeight.Distribution, 0.0f, HeightMin, HeightMax, bCurve))
					{
						S.Shape = EMantleSpritesShape::Cylinder;
						S.CylinderRadius = RadiusMax;
						S.CylinderHeight = HeightMax;
						bLocation = true;
						if (Cylinder->HeightAxis != PMLPC_HEIGHTAXIS_Z)
						{
							Out.Approximations.Add(TEXT("a cylinder along X or Y stands up along Z"));
						}
						if (RadiusMin != RadiusMax || HeightMin != HeightMax)
						{
							Out.Approximations.Add(TEXT("a cylinder's radius and height ranges take their largest"));
						}
					}
				}
				else
				{
					Out.Ignored.Add(ModuleLabel(Module));
				}
			}
			else if (const UParticleModuleLocationPrimitiveSphere* Sphere = Cast<UParticleModuleLocationPrimitiveSphere>(Module))
			{
				if (!bLocation)
				{
					double RadiusMin = 0.0, RadiusMax = 0.0;
					if (SampleFloat(Sphere->StartRadius.Distribution, 0.0f, RadiusMin, RadiusMax, bCurve))
					{
						S.Shape = EMantleSpritesShape::Sphere;
						S.SphereRadius = RadiusMax;
						bLocation = true;
						if (RadiusMin != RadiusMax)
						{
							Out.Approximations.Add(TEXT("a sphere's radius range takes its largest"));
						}
					}
				}
				else
				{
					Out.Ignored.Add(ModuleLabel(Module));
				}
			}
			else if (const UParticleModuleLocation* Location = Cast<UParticleModuleLocation>(Module))
			{
				// A start location spread over a range is a box; a single point is the effect's own origin.
				FVector Min, Max;
				if (!bLocation && SampleVector(Location->StartLocation.Distribution, 0.0f, Min, Max, bCurve))
				{
					const FVector Half = (Max - Min) * 0.5;
					if (!Half.IsNearlyZero())
					{
						S.Shape = EMantleSpritesShape::Box;
						S.BoxHalfExtent = Half;
						bLocation = true;
					}
					if (!(Max + Min).IsNearlyZero())
					{
						Out.Approximations.Add(TEXT("a start location's offset from the origin dropped"));
					}
				}
				else
				{
					Out.Ignored.Add(ModuleLabel(Module));
				}
			}
			else if (const UParticleModuleColorOverLife* ColorOverLife = Cast<UParticleModuleColorOverLife>(Module))
			{
				FVector Start, End, Unused;
				bool bEndCurve = false;
				if (SampleVector(ColorOverLife->ColorOverLife.Distribution, 0.0f, Start, Unused, bCurve)
					&& SampleVector(ColorOverLife->ColorOverLife.Distribution, 1.0f, End, Unused, bEndCurve))
				{
					Props.Look.Color = FLinearColor(FMath::Clamp(Start.X, 0.0, 1.0), FMath::Clamp(Start.Y, 0.0, 1.0), FMath::Clamp(Start.Z, 0.0, 1.0));
					S.ColorEnd = FLinearColor(FMath::Clamp(End.X, 0.0, 1.0), FMath::Clamp(End.Y, 0.0, 1.0), FMath::Clamp(End.Z, 0.0, 1.0));
					bColorOverLife = true;
					if (bCurve || bEndCurve)
					{
						Out.Approximations.Add(TEXT("colour over life reduced to its start and end"));
					}
				}
				// Alpha over life: its start, its peak and its end, so a fade in and out survives.
				double A0 = 0.0, A1 = 0.0, Peak = 0.0, PeakTime = 0.0, Ignored = 0.0;
				bool bAlphaCurve = false;
				if (SampleFloat(ColorOverLife->AlphaOverLife.Distribution, 0.0f, A0, Ignored, bAlphaCurve)
					&& SampleFloat(ColorOverLife->AlphaOverLife.Distribution, 1.0f, A1, Ignored, bCurve))
				{
					constexpr int32 Samples = 16;
					Peak = A0;
					for (int32 Step = 1; Step <= Samples; ++Step)
					{
						double Value = 0.0;
						const float T = static_cast<float>(Step) / Samples;
						SampleFloat(ColorOverLife->AlphaOverLife.Distribution, T, Value, Ignored, bCurve);
						if (Value > Peak + 1.0e-6)
						{
							Peak = Value;
							PeakTime = T;
						}
					}
					const bool bFadeIn = A0 < 0.5 * Peak && PeakTime > 0.05 && PeakTime < 0.95;
					S.Alpha = FMath::Clamp(bFadeIn ? Peak : A0, 0.0, 1.0);
					S.FadeIn = bFadeIn ? PeakTime : 0.0;
					S.AlphaEnd = FMath::Clamp(A1, 0.0, 1.0);
					bAlphaOverLife = true;
					if (bAlphaCurve)
					{
						Out.Approximations.Add(TEXT("alpha over life reduced to a fade in, a peak and an end"));
					}
				}
			}
			else if (const UParticleModuleColor* Color = Cast<UParticleModuleColor>(Module))
			{
				FVector Min, Max;
				double AlphaMin = 0.0, AlphaMax = 0.0;
				if (!bColorOverLife && SampleVector(Color->StartColor.Distribution, 0.0f, Min, Max, bCurve))
				{
					const FVector Mean = (Min + Max) * 0.5;
					Props.Look.Color = FLinearColor(FMath::Clamp(Mean.X, 0.0, 1.0), FMath::Clamp(Mean.Y, 0.0, 1.0), FMath::Clamp(Mean.Z, 0.0, 1.0));
					S.ColorEnd = Props.Look.Color;
				}
				if (!bAlphaOverLife && SampleFloat(Color->StartAlpha.Distribution, 0.0f, AlphaMin, AlphaMax, bCurve))
				{
					S.Alpha = FMath::Clamp(Average(AlphaMin, AlphaMax), 0.0, 1.0);
					S.AlphaEnd = S.Alpha;
				}
			}
			else if (const UParticleModuleAccelerationConstant* Constant = Cast<UParticleModuleAccelerationConstant>(Module))
			{
				S.Acceleration += Constant->Acceleration;
			}
			else if (const UParticleModuleAcceleration* Acceleration = Cast<UParticleModuleAcceleration>(Module))
			{
				FVector Min, Max;
				if (SampleVector(Acceleration->Acceleration.Distribution, 0.0f, Min, Max, bCurve))
				{
					S.Acceleration += (Min + Max) * 0.5;
				}
			}
			else if (const UParticleModuleSubUV* SubUVModule = Cast<UParticleModuleSubUV>(Module))
			{
				// A flipbook sheet (a movie module is a SubUV module): worked out below, with the Required module's sheet and method.
				if (!SubUV)
				{
					SubUV = SubUVModule;
				}
				else
				{
					Out.Ignored.Add(ModuleLabel(Module));
				}
			}
			else
			{
				Out.Ignored.Add(ModuleLabel(Module));
			}
		}
		if (SubUV && !ReadSubImages(Required, SubUV, S, Out))
		{
			Out.Ignored.Add(ModuleLabel(SubUV));
		}
		if (bCurveUsed)
		{
			Out.Approximations.Add(TEXT("curves read at their start and end"));
		}
		if (!bSizeStart)
		{
			Out.Approximations.Add(TEXT("no start size, so 25 cm"));
		}

		const FMantleSpritesSettings Before = S;
		const double RateBefore = Props.Look.Rate;
		S =FMantleEffects::ClampSprites(S);
		Props.Look.Rate = FMath::Clamp(Props.Look.Rate, 0.0, FMantlePlaceLimits::MaxEffectRate);
		const bool bClamped = !Nearly(Before.SizeMin, S.SizeMin) || !Nearly(Before.SizeMax, S.SizeMax) || !Nearly(Before.LifetimeMin, S.LifetimeMin)
			|| !Nearly(Before.LifetimeMax, S.LifetimeMax) || !NearlyVector(Before.VelocityMin, S.VelocityMin) || !NearlyVector(Before.VelocityMax, S.VelocityMax)
			|| !NearlyVector(Before.Acceleration, S.Acceleration) || !Nearly(Before.SizeEndScale, S.SizeEndScale) || !Nearly(RateBefore, Props.Look.Rate)
			|| !NearlyVector(Before.BoxHalfExtent, S.BoxHalfExtent) || !Nearly(Before.CylinderRadius, S.CylinderRadius)
			|| !Nearly(Before.CylinderHeight, S.CylinderHeight) || !Nearly(Before.SphereRadius, S.SphereRadius);
		if (bClamped)
		{
			Out.Approximations.Add(TEXT("values held to Mantle's limits"));
		}
		return true;
	}

	/** Every emitter of a Cascade component, translated; the loss reports of the ones it cannot carry already on Coverage. */
	void TranslateCascade(const UParticleSystemComponent* Component, const FString& Label, FMantleCoverage& Coverage, TArray<FTranslatedEmitter>& Out)
	{
		const UParticleSystem* System = Component->Template;
		if (!System)
		{
			return;
		}
		int32 Index = 0;
		for (UParticleEmitter* Emitter : System->Emitters)
		{
			const FString Name = System->Emitters.Num() > 1
				? FString::Printf(TEXT("%s (emitter %d %s)"), *Label, Index, Emitter ? *Emitter->GetEmitterName().ToString() : TEXT(""))
				: Label;
			++Index;
			FTranslatedEmitter Translated;
			if (!TranslateEmitter(Emitter, Component, Name, Coverage, Translated))
			{
				continue;
			}
			const bool bApproximate = Translated.Approximations.Num() > 0 || Translated.Ignored.Num() > 0;
			FString Detail = bApproximate ? FString() : TEXT("sprites");
			if (bApproximate)
			{
				Detail = TEXT("sprites, approximated");
				if (Translated.Approximations.Num() > 0)
				{
					Detail += TEXT(": ") + FString::Join(Translated.Approximations, TEXT("; "));
				}
				if (Translated.Ignored.Num() > 0)
				{
					Detail += FString::Printf(TEXT("%s ignored modules: %s"), Translated.Approximations.Num() > 0 ? TEXT(";") : TEXT(":"),
						*FString::Join(Translated.Ignored, TEXT(", ")));
				}
			}
			Coverage.Add(MantleCoverageCategory::Particles, bApproximate ? EMantleCoverageOutcome::Approximated : EMantleCoverageOutcome::Kept, Name, Detail);
			Out.Add(MoveTemp(Translated));
		}
	}

	void WriteVector(const TSharedRef<FJsonObject>& Object, const TCHAR* Key, const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Add(MakeShared<FJsonValueNumber>(Value.X));
		Array.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Array.Add(MakeShared<FJsonValueNumber>(Value.Z));
		Object->SetArrayField(Key, Array);
	}

	void WriteColor(const TSharedRef<FJsonObject>& Object, const TCHAR* Key, const FLinearColor& Value)
	{
		WriteVector(Object, Key, FVector(Value.R, Value.G, Value.B));
	}

	/** One translated emitter as a place Effect "properties" object: every sprites key, the texture as its object path. */
	TSharedPtr<FJsonObject> ToJson(const FTranslatedEmitter& Emitter)
	{
		const FMantleEffectProps& Props = Emitter.Props;
		const FMantleSpritesSettings& S = Props.Sprites;
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("archetype"), FMantleEffects::ArchetypeName(Props.Archetype));
		if (!Emitter.TexturePath.IsEmpty())
		{
			Object->SetStringField(TEXT("texture"), Emitter.TexturePath);
		}
		Object->SetStringField(TEXT("blend"), FMantleEffects::BlendName(S.Blend));
		WriteColor(Object, TEXT("color"), Props.Look.Color);
		WriteColor(Object, TEXT("colorEnd"), S.ColorEnd);
		Object->SetNumberField(TEXT("alpha"), S.Alpha);
		Object->SetNumberField(TEXT("alphaEnd"), S.AlphaEnd);
		Object->SetNumberField(TEXT("fadeIn"), S.FadeIn);
		Object->SetNumberField(TEXT("sizeMin"), S.SizeMin);
		Object->SetNumberField(TEXT("sizeMax"), S.SizeMax);
		Object->SetNumberField(TEXT("sizeEndScale"), S.SizeEndScale);
		Object->SetNumberField(TEXT("rate"), Props.Look.Rate);
		Object->SetNumberField(TEXT("lifetimeMin"), S.LifetimeMin);
		Object->SetNumberField(TEXT("lifetimeMax"), S.LifetimeMax);
		WriteVector(Object, TEXT("velocityMin"), S.VelocityMin);
		WriteVector(Object, TEXT("velocityMax"), S.VelocityMax);
		WriteVector(Object, TEXT("acceleration"), S.Acceleration);
		const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
		Shape->SetStringField(TEXT("type"), FMantleEffects::ShapeName(S.Shape));
		switch (S.Shape)
		{
		case EMantleSpritesShape::Box:
			WriteVector(Shape, TEXT("halfExtent"), S.BoxHalfExtent);
			break;
		case EMantleSpritesShape::Sphere:
			Shape->SetNumberField(TEXT("radius"), S.SphereRadius);
			break;
		case EMantleSpritesShape::Cylinder:
			Shape->SetNumberField(TEXT("radius"), S.CylinderRadius);
			Shape->SetNumberField(TEXT("height"), S.CylinderHeight);
			break;
		case EMantleSpritesShape::Point:
			break;
		}
		Object->SetObjectField(TEXT("shape"), Shape);
		Object->SetNumberField(TEXT("maxParticles"), S.MaxParticles);
		if (S.SubImageColumns > 1 || S.SubImageRows > 1)
		{
			// Only a sheet is written: the schema's default is the whole texture.
			TArray<TSharedPtr<FJsonValue>> Sheet;
			Sheet.Add(MakeShared<FJsonValueNumber>(S.SubImageColumns));
			Sheet.Add(MakeShared<FJsonValueNumber>(S.SubImageRows));
			Object->SetArrayField(TEXT("subImages"), Sheet);
			Object->SetStringField(TEXT("subImageMode"), FMantleEffects::SubImageModeName(S.SubImageMode));
		}
		Object->SetBoolField(TEXT("autoplay"), Props.bAutoplay);
		Object->SetBoolField(TEXT("loop"), Props.bLoop);
		return Object;
	}

	bool ReadNumberKey(const FJsonObject& Object, const TCHAR* Key, double& Out)
	{
		double Value = 0.0;
		if (Object.TryGetNumberField(Key, Value) && FMath::IsFinite(Value))
		{
			Out = Value;
			return true;
		}
		return false;
	}

	bool ReadVectorKey(const FJsonObject& Object, const TCHAR* Key, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object.TryGetArrayField(Key, Array) || !Array || Array->Num() < 3)
		{
			return false;
		}
		Out = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
		return true;
	}
}

bool FMantleEffectExport::ReadFloat(const UDistributionFloat* Distribution, double& OutMin, double& OutMax, bool& bOutApproximated)
{
	using namespace MantleEffectExportPrivate;
	bool bCurve = false;
	double StartMin = 0.0, StartMax = 0.0, EndMin = 0.0, EndMax = 0.0;
	if (!SampleFloat(Distribution, 0.0f, StartMin, StartMax, bCurve))
	{
		return false;
	}
	bool bEndCurve = false;
	SampleFloat(Distribution, 1.0f, EndMin, EndMax, bEndCurve);
	bOutApproximated = bCurve;
	OutMin = bCurve ? FMath::Min(StartMin, EndMin) : StartMin;
	OutMax = bCurve ? FMath::Max(StartMax, EndMax) : StartMax;
	return true;
}

bool FMantleEffectExport::ReadVector(const UDistributionVector* Distribution, FVector& OutMin, FVector& OutMax, bool& bOutApproximated)
{
	using namespace MantleEffectExportPrivate;
	bool bCurve = false;
	FVector StartMin, StartMax, EndMin, EndMax;
	if (!SampleVector(Distribution, 0.0f, StartMin, StartMax, bCurve))
	{
		return false;
	}
	bool bEndCurve = false;
	SampleVector(Distribution, 1.0f, EndMin, EndMax, bEndCurve);
	bOutApproximated = bCurve;
	OutMin = bCurve ? StartMin.ComponentMin(EndMin) : StartMin;
	OutMax = bCurve ? StartMax.ComponentMax(EndMax) : StartMax;
	return true;
}

bool FMantleEffectExport::Translate(const UFXSystemComponent* Component, const FString& Label, FMantleCoverage& Coverage,
	TArray<TSharedPtr<FJsonObject>>& OutEffectProperties)
{
	using namespace MantleEffectExportPrivate;
	if (!Component)
	{
		return false;
	}
	const UParticleSystemComponent* Cascade = Cast<UParticleSystemComponent>(Component);
	if (!Cascade)
	{
		// A Niagara system (the only other UFXSystemComponent): reported, not an error.
		Coverage.Add(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped, Label, TEXT("a Niagara system: Niagara systems are next"));
		return false;
	}
	if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::EffectSprites))
	{
		Coverage.Add(MantleCoverageCategory::Particles, EMantleCoverageOutcome::Dropped, Label,
			FString::Printf(TEXT("a particle system left out: %s"), FMantleTargetFeatures::NeedsNewerClient));
		return false;
	}
	TArray<FTranslatedEmitter> Emitters;
	TranslateCascade(Cascade, Label, Coverage, Emitters);
	for (const FTranslatedEmitter& Emitter : Emitters)
	{
		OutEffectProperties.Add(ToJson(Emitter));
	}
	return Emitters.Num() > 0;
}

UTexture* FMantleEffectExport::TextureForPath(const FString& Path)
{
	return IsTexturePath(Path) ? LoadObject<UTexture>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
}

bool FMantleEffectExport::ReadProperties(const FJsonObject& Properties, FMantleEffectProps& Out)
{
	using namespace MantleEffectExportPrivate;
	FString Archetype;
	if (!Properties.TryGetStringField(TEXT("archetype"), Archetype) || !FMantleEffects::ParseArchetype(Archetype, Out.Archetype))
	{
		return false;
	}
	Out.Look = FMantleEffects::DefaultLook(Out.Archetype);
	FVector Color;
	if (ReadVectorKey(Properties, TEXT("color"), Color))
	{
		Out.Look.Color = FLinearColor(static_cast<float>(Color.X), static_cast<float>(Color.Y), static_cast<float>(Color.Z));
	}
	ReadNumberKey(Properties, TEXT("size"), Out.Look.Size);
	ReadNumberKey(Properties, TEXT("rate"), Out.Look.Rate);
	ReadNumberKey(Properties, TEXT("speed"), Out.Look.Speed);
	ReadNumberKey(Properties, TEXT("lifetime"), Out.Look.Lifetime);
	bool bFlag = true;
	if (Properties.TryGetBoolField(TEXT("autoplay"), bFlag))
	{
		Out.bAutoplay = bFlag;
	}
	if (Properties.TryGetBoolField(TEXT("loop"), bFlag))
	{
		Out.bLoop = bFlag;
	}
	if (Out.Archetype != EMantleEffectArchetype::Sprites)
	{
		return true;
	}

	FMantleSpritesSettings& S = Out.Sprites;
	S = FMantleSpritesSettings();
	Properties.TryGetStringField(TEXT("texture"), Out.TextureId);
	FString Text;
	if (Properties.TryGetStringField(TEXT("blend"), Text))
	{
		FMantleEffects::ParseBlend(Text, S.Blend);
	}
	S.ColorEnd = Out.Look.Color;
	if (ReadVectorKey(Properties, TEXT("colorEnd"), Color))
	{
		S.ColorEnd = FLinearColor(static_cast<float>(Color.X), static_cast<float>(Color.Y), static_cast<float>(Color.Z));
	}
	ReadNumberKey(Properties, TEXT("alpha"), S.Alpha);
	ReadNumberKey(Properties, TEXT("alphaEnd"), S.AlphaEnd);
	ReadNumberKey(Properties, TEXT("fadeIn"), S.FadeIn);
	ReadNumberKey(Properties, TEXT("sizeEndScale"), S.SizeEndScale);
	const bool bSizeMin = ReadNumberKey(Properties, TEXT("sizeMin"), S.SizeMin);
	const bool bSizeMax = ReadNumberKey(Properties, TEXT("sizeMax"), S.SizeMax);
	FMantleEffects::ResolveSpritesPair(bSizeMin, bSizeMax, S.SizeMin, S.SizeMax);
	const bool bLifetimeMin = ReadNumberKey(Properties, TEXT("lifetimeMin"), S.LifetimeMin);
	const bool bLifetimeMax = ReadNumberKey(Properties, TEXT("lifetimeMax"), S.LifetimeMax);
	FMantleEffects::ResolveSpritesPair(bLifetimeMin, bLifetimeMax, S.LifetimeMin, S.LifetimeMax);
	const bool bVelocityMin = ReadVectorKey(Properties, TEXT("velocityMin"), S.VelocityMin);
	const bool bVelocityMax = ReadVectorKey(Properties, TEXT("velocityMax"), S.VelocityMax);
	FMantleEffects::ResolveSpritesVectorPair(bVelocityMin, bVelocityMax, S.VelocityMin, S.VelocityMax);
	ReadVectorKey(Properties, TEXT("acceleration"), S.Acceleration);
	double Count = 0.0;
	if (ReadNumberKey(Properties, TEXT("maxParticles"), Count))
	{
		S.MaxParticles = static_cast<int32>(FMath::Clamp(Count, 1.0, static_cast<double>(FMantlePlaceLimits::MaxSpriteParticles)));
	}
	const TArray<TSharedPtr<FJsonValue>>* Sheet = nullptr;
	if (Properties.TryGetArrayField(TEXT("subImages"), Sheet) && Sheet && Sheet->Num() >= 2)
	{
		S.SubImageColumns = static_cast<int32>(FMath::Clamp((*Sheet)[0]->AsNumber(), 1.0, static_cast<double>(FMantlePlaceLimits::MaxSpriteSubImages)));
		S.SubImageRows = static_cast<int32>(FMath::Clamp((*Sheet)[1]->AsNumber(), 1.0, static_cast<double>(FMantlePlaceLimits::MaxSpriteSubImages)));
	}
	if (Properties.TryGetStringField(TEXT("subImageMode"), Text))
	{
		FMantleEffects::ParseSubImageMode(Text, S.SubImageMode);
	}
	const TSharedPtr<FJsonObject>* Shape = nullptr;
	if (Properties.TryGetObjectField(TEXT("shape"), Shape) && Shape && Shape->IsValid())
	{
		if ((*Shape)->TryGetStringField(TEXT("type"), Text))
		{
			FMantleEffects::ParseShape(Text, S.Shape);
		}
		ReadVectorKey(**Shape, TEXT("halfExtent"), S.BoxHalfExtent);
		if (S.Shape == EMantleSpritesShape::Sphere)
		{
			ReadNumberKey(**Shape, TEXT("radius"), S.SphereRadius);
		}
		else
		{
			ReadNumberKey(**Shape, TEXT("radius"), S.CylinderRadius);
		}
		ReadNumberKey(**Shape, TEXT("height"), S.CylinderHeight);
	}
	S = FMantleEffects::ClampSprites(S);
	return true;
}
