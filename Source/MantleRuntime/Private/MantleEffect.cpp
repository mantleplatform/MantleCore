// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEffect.h"

#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"

#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreMisc.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#if WITH_EDITORONLY_DATA
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#endif

// The Details panel's ClampMin/ClampMax metas are literals; these keep them on the place limits.
static_assert(FMantlePlaceLimits::MinEffectSize == 1.0 && FMantlePlaceLimits::MaxEffectSize == 1000.0,
	"update AMantleEffect::Size's metas");
static_assert(FMantlePlaceLimits::MaxEffectRate == 200.0, "update AMantleEffect::Rate's metas");
static_assert(FMantlePlaceLimits::MaxEffectSpeed == 5000.0, "update AMantleEffect::Speed's metas");
static_assert(FMantlePlaceLimits::MinEffectLifetime == 0.05 && FMantlePlaceLimits::MaxEffectLifetime == 10.0,
	"update AMantleEffect::Lifetime's metas");

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleEffectActorPrivate
{
	/** Inside [Min, Max]; written so a NaN is outside. */
	bool IsEffectInRange(const double Value, const double Min, const double Max)
	{
		return Value >= Min && Value <= Max;
	}

	/**
	 * Points Component at System through Niagara's own SetAsset (a UFUNCTION, called by reflection so
	 * MantleCore links no plugin): unlike writing its Asset property, that also takes the system's user
	 * parameters into the component. False when the class has no such function taking the system.
	 */
	bool SetEffectComponentSystem(UFXSystemComponent& Component, UFXSystemAsset& System)
	{
		UFunction* Function = Component.FindFunction(TEXT("SetAsset"));
		if (!Function)
		{
			return false;
		}
		FStructOnScope Params(Function);
		uint8* Memory = Params.GetStructMemory();
		bool bSetSystem = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(*It))
			{
				if (!bSetSystem && System.IsA(Object->PropertyClass))
				{
					Object->SetObjectPropertyValue_InContainer(Memory, &System);
					bSetSystem = true;
				}
			}
			else if (FBoolProperty* Flag = CastField<FBoolProperty>(*It))
			{
				Flag->SetPropertyValue_InContainer(Memory, true); // bResetExistingOverrideParameters
			}
		}
		if (!bSetSystem)
		{
			return false;
		}
		Component.ProcessEvent(Function, Memory);
		return true;
	}

	/**
	 * Writes Value to Component's Vector2D user parameter Name through Niagara's SetVariableVec2 (a
	 * UFUNCTION, called by reflection so MantleCore links no plugin). False when the class has no such
	 * function taking a name and a FVector2D.
	 */
	bool SetEffectComponentVec2(UFXSystemComponent& Component, const FName Name, const FVector2D& Value)
	{
		UFunction* Function = Component.FindFunction(TEXT("SetVariableVec2"));
		if (!Function)
		{
			return false;
		}
		FStructOnScope Params(Function);
		uint8* Memory = Params.GetStructMemory();
		bool bSetName = false;
		bool bSetValue = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (FNameProperty* NameProperty = CastField<FNameProperty>(*It))
			{
				NameProperty->SetPropertyValue_InContainer(Memory, Name);
				bSetName = true;
			}
			else if (FStructProperty* StructProperty = CastField<FStructProperty>(*It))
			{
				if (StructProperty->Struct == TBaseStructure<FVector2D>::Get())
				{
					*StructProperty->ContainerPtrToValuePtr<FVector2D>(Memory) = Value;
					bSetValue = true;
				}
			}
		}
		if (!bSetName || !bSetValue)
		{
			return false;
		}
		Component.ProcessEvent(Function, Memory);
		return true;
	}

	/**
	 * Writes Value to Component's Vector3 (or position) user parameter Name through the UFUNCTION FunctionName taking a name
	 * and an FVector (Niagara's SetVariableVec3 and SetVariablePosition), called by reflection. False when the class has none.
	 */
	bool SetEffectComponentVec3(UFXSystemComponent& Component, const TCHAR* FunctionName, const FName Name, const FVector& Value)
	{
		UFunction* Function = Component.FindFunction(FunctionName);
		if (!Function)
		{
			return false;
		}
		FStructOnScope Params(Function);
		uint8* Memory = Params.GetStructMemory();
		bool bSetName = false;
		bool bSetValue = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (FNameProperty* NameProperty = CastField<FNameProperty>(*It))
			{
				NameProperty->SetPropertyValue_InContainer(Memory, Name);
				bSetName = true;
			}
			else if (FStructProperty* StructProperty = CastField<FStructProperty>(*It))
			{
				if (StructProperty->Struct == TBaseStructure<FVector>::Get())
				{
					*StructProperty->ContainerPtrToValuePtr<FVector>(Memory) = Value;
					bSetValue = true;
				}
			}
		}
		if (!bSetName || !bSetValue)
		{
			return false;
		}
		Component.ProcessEvent(Function, Memory);
		return true;
	}

	/**
	 * Writes Material to Component's material user parameter Name through Niagara's SetVariableMaterial (a UFUNCTION,
	 * called by reflection so MantleCore links no plugin). False when the class has no such function.
	 */
	bool SetEffectComponentMaterial(UFXSystemComponent& Component, const FName Name, UMaterialInterface* Material)
	{
		UFunction* Function = Component.FindFunction(TEXT("SetVariableMaterial"));
		if (!Function)
		{
			return false;
		}
		FStructOnScope Params(Function);
		uint8* Memory = Params.GetStructMemory();
		bool bSetName = false;
		bool bSetMaterial = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (FNameProperty* NameProperty = CastField<FNameProperty>(*It))
			{
				NameProperty->SetPropertyValue_InContainer(Memory, Name);
				bSetName = true;
			}
			else if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(*It))
			{
				Object->SetObjectPropertyValue_InContainer(Memory, Material);
				bSetMaterial = true;
			}
		}
		if (!bSetName || !bSetMaterial)
		{
			return false;
		}
		Component.ProcessEvent(Function, Memory);
		return true;
	}

	/** Sets a sprites material instance's values: the texture (or the soft dot), the colours, the opacity envelope, the end scale and the flipbook sheet. */
	void WriteSpritesMaterialValues(UMaterialInstanceDynamic& Material, const FMantleEffectLook& Look, const FMantleSpritesSettings& Sprites, UTexture* Texture)
	{
		Material.SetScalarParameterValue(FName(FMantleEffects::UseTextureParameter), Texture ? 1.0f : 0.0f);
		if (Texture)
		{
			Material.SetTextureParameterValue(FName(FMantleEffects::TextureParameter), Texture);
		}
		Material.SetVectorParameterValue(FName(FMantleEffects::ColorStartMaterialParameter), FLinearColor(Look.Color.R, Look.Color.G, Look.Color.B, 1.0f));
		Material.SetVectorParameterValue(FName(FMantleEffects::ColorEndMaterialParameter), FLinearColor(Sprites.ColorEnd.R, Sprites.ColorEnd.G, Sprites.ColorEnd.B, 1.0f));
		Material.SetScalarParameterValue(FName(FMantleEffects::AlphaParameter), static_cast<float>(Sprites.Alpha));
		Material.SetScalarParameterValue(FName(FMantleEffects::AlphaEndParameter), static_cast<float>(Sprites.AlphaEnd));
		Material.SetScalarParameterValue(FName(FMantleEffects::FadeInParameter), static_cast<float>(Sprites.FadeIn));
		Material.SetScalarParameterValue(FName(FMantleEffects::SizeEndScaleParameter), static_cast<float>(Sprites.SizeEndScale));
		// A flipbook sheet: columns and rows (1 by 1 is the whole texture, as before), and whether the frames play over a life.
		Material.SetVectorParameterValue(FName(FMantleEffects::SubImagesParameter),
			FLinearColor(static_cast<float>(FMath::Max(Sprites.SubImageColumns, 1)), static_cast<float>(FMath::Max(Sprites.SubImageRows, 1)), 0.0f, 0.0f));
		Material.SetScalarParameterValue(FName(FMantleEffects::SubImageOverLifeParameter), Sprites.SubImageMode == EMantleSpritesSubImageMode::OverLife ? 1.0f : 0.0f);
	}
}

AMantleEffect::AMantleEffect()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	EffectRoot = CreateDefaultSubobject<USceneComponent>(TEXT("EffectRoot"));
	SetRootComponent(EffectRoot);

	const FMantleEffectLook Own = FMantleEffects::DefaultLook(Archetype);
	Color = Own.Color;
	Size = Own.Size;
	Rate = Own.Rate;
	Speed = Own.Speed;
	Lifetime = Own.Lifetime;

#if WITH_EDITORONLY_DATA
	EditorBillboard = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("EditorBillboard"));
	if (EditorBillboard)
	{
		EditorBillboard->SetupAttachment(EffectRoot);
		// The engine's own particle emitter icon; cosmetic only.
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> EmitterIcon(TEXT("/Engine/EditorResources/S_Emitter"));
		EditorBillboard->Sprite = EmitterIcon.Get();
		EditorBillboard->bIsScreenSizeScaled = true;
		EditorBillboard->SetIsVisualizationComponent(true);
		EditorBillboard->SetHiddenInGame(true);
	}
#endif
}

bool AMantleEffect::DrawsEffects(const UWorld* World)
{
#if UE_SERVER
	(void)World;
	return false;
#else
	return World && World->GetNetMode() != NM_DedicatedServer && !IsRunningDedicatedServer();
#endif
}

UFXSystemComponent* AMantleEffect::GetEffectComponent() const
{
	return EffectComponent;
}

bool AMantleEffect::IsEditorWorld() const
{
	const UWorld* World = GetWorld();
	return World && !World->IsGameWorld();
}

void AMantleEffect::ApplyProps(const FMantleEffectProps& In)
{
	Archetype = In.Archetype;
	Color = In.Look.Color;
	Size = In.Look.Size;
	Rate = In.Look.Rate;
	Speed = In.Look.Speed;
	Lifetime = In.Look.Lifetime;
	bAutoplay = In.bAutoplay;
	bLoop = In.bLoop;
	Sprites = In.Sprites;
	if (IsEditorWorld())
	{
		RefreshEditorPreview();
	}
	else if (EffectComponent)
	{
		// A different archetype replaces the component; the same one only takes the new look.
		if (EnsureEffectComponent())
		{
			PushLook();
		}
	}
}

bool AMantleEffect::ReadProps(FMantleEffectProps& Out, FString& OutProblem) const
{
	using namespace MantleEffectActorPrivate;
	Out.Archetype = Archetype;
	Out.Look = GetLook();
	Out.bAutoplay = bAutoplay;
	Out.bLoop = bLoop;
	Out.Sprites = FMantleEffects::ClampSprites(Sprites);
	if (!IsEffectInRange(Color.R, 0.0, 1.0) || !IsEffectInRange(Color.G, 0.0, 1.0) || !IsEffectInRange(Color.B, 0.0, 1.0))
	{
		OutProblem = FString::Printf(TEXT("has a color of (%g, %g, %g); set each of Color's R, G and B between 0 and 1"),
			Color.R, Color.G, Color.B);
		return false;
	}
	if (!IsEffectInRange(Size, FMantlePlaceLimits::MinEffectSize, FMantlePlaceLimits::MaxEffectSize))
	{
		OutProblem = FString::Printf(TEXT("has particles %g cm across; set Size between %g and %g"),
			Size, FMantlePlaceLimits::MinEffectSize, FMantlePlaceLimits::MaxEffectSize);
		return false;
	}
	if (!IsEffectInRange(Rate, 0.0, FMantlePlaceLimits::MaxEffectRate))
	{
		OutProblem = FString::Printf(TEXT("spawns %g particles a second; set Rate between 0 and %g"), Rate, FMantlePlaceLimits::MaxEffectRate);
		return false;
	}
	if (!IsEffectInRange(Speed, 0.0, FMantlePlaceLimits::MaxEffectSpeed))
	{
		OutProblem = FString::Printf(TEXT("moves its particles at %g cm/s; set Speed between 0 and %g"), Speed, FMantlePlaceLimits::MaxEffectSpeed);
		return false;
	}
	if (!IsEffectInRange(Lifetime, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime))
	{
		OutProblem = FString::Printf(TEXT("keeps its particles for %g s; set Lifetime between %g and %g"),
			Lifetime, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime);
		return false;
	}
	return true;
}

FMantleEffectLook AMantleEffect::GetLook() const
{
	FMantleEffectLook Look;
	Look.Color = FLinearColor(Color.R, Color.G, Color.B);
	Look.Size = Size;
	Look.Rate = Rate;
	Look.Speed = Speed;
	Look.Lifetime = Lifetime;
	return Look;
}

void AMantleEffect::BeginPlaceEffect()
{
	if (IsEditorWorld())
	{
		RefreshEditorPreview();
		return;
	}
	if (bAutoplay)
	{
		PlayEffect();
	}
}

void AMantleEffect::PlayEffect()
{
	if (bPlaying && bLoop)
	{
		return; // a looping effect already playing keeps going
	}
	bPlaying = true;
	if (EnsureEffectComponent())
	{
		PushLook();
		EffectComponent->Activate(/*bReset*/ true);
		if (Archetype == EMantleEffectArchetype::Sprites)
		{
			// The volume emitters are switched on a running system: say which again once it is active.
			PushLook();
		}
	}
	if (bLoop)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(BurstTimer);
		}
	}
	else
	{
		ScheduleBurstEnd(FMantleEffects::OneShotEmitSeconds);
	}
}

void AMantleEffect::StopEffect()
{
	bPlaying = false;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BurstTimer);
	}
	if (EffectComponent)
	{
		EffectComponent->Deactivate();
	}
}

void AMantleEffect::SetEffectColor(const FLinearColor& InColor)
{
	const FMantleEffectLook Clamped = FMantleEffects::ClampLook(FMantleEffectLook{ InColor, Size, Rate, Speed, Lifetime }, Archetype);
	Color = Clamped.Color;
	PushLook();
}

void AMantleEffect::SetEffectRate(const double InRate)
{
	FMantleEffectLook Look = GetLook();
	Look.Rate = InRate;
	Rate = FMantleEffects::ClampLook(Look, Archetype).Rate;
	PushLook();
}

AMantleEffect* AMantleEffect::SpawnOneShot(UWorld* World, const FTransform& Transform, const EMantleEffectArchetype InArchetype,
	const FMantleEffectLook& InLook, const double EmitSeconds)
{
	// Nothing to show here: spawn nothing (a server's scripts send one-shots to the players instead).
	if (!DrawsEffects(World) || !FMantleEffects::ComponentClass() || !FMantleEffects::LoadSystem(InArchetype))
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	AMantleEffect* Effect = World->SpawnActor<AMantleEffect>(AMantleEffect::StaticClass(), Transform, Params);
	if (!Effect)
	{
		return nullptr;
	}
	const FMantleEffectLook Look = FMantleEffects::ClampLook(InLook, InArchetype);
	Effect->Archetype = InArchetype;
	Effect->Color = Look.Color;
	Effect->Size = Look.Size;
	Effect->Rate = Look.Rate;
	Effect->Speed = Look.Speed;
	Effect->Lifetime = Look.Lifetime;
	Effect->bAutoplay = false;
	// Emits for EmitSeconds rather than the place's one-second burst, then goes once its last particles have.
	Effect->bLoop = true;
	Effect->PlayEffect();
	const double Emit = FMath::Max(0.0, EmitSeconds);
	Effect->ScheduleBurstEnd(Emit);
	Effect->SetLifeSpan(static_cast<float>(Emit + Look.Lifetime + 0.5));
	return Effect;
}

UFXSystemComponent* AMantleEffect::SpawnSystem(UWorld* World, const FTransform& Transform, UFXSystemAsset* System, const double LifeSeconds)
{
	UClass* ComponentClass = FMantleEffects::ComponentClass();
	if (!DrawsEffects(World) || !ComponentClass || !System)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	AMantleEffect* Host = World->SpawnActor<AMantleEffect>(AMantleEffect::StaticClass(), Transform, Params);
	if (!Host)
	{
		return nullptr;
	}
	Host->bAutoplay = false;
	UFXSystemComponent* Component = NewObject<UFXSystemComponent>(Host, ComponentClass, NAME_None,
		RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
	if (!Component || !MantleEffectActorPrivate::SetEffectComponentSystem(*Component, *System))
	{
		if (Component)
		{
			Component->MarkAsGarbage();
		}
		Host->Destroy();
		return nullptr;
	}
	Component->SetAutoActivate(false);
	Component->SetupAttachment(Host->EffectRoot);
	Component->RegisterComponent();
	// Left inactive: the caller writes its user parameters, then activates. The actor outlives the system by a moment.
	Host->SetLifeSpan(static_cast<float>(FMath::Max(LifeSeconds, 0.05) + 0.5));
	return Component;
}

bool AMantleEffect::SetSystemVariableVec3(UFXSystemComponent& Component, const TCHAR* FunctionName, const FName Name, const FVector& Value)
{
	return MantleEffectActorPrivate::SetEffectComponentVec3(Component, FunctionName, Name, Value);
}

void AMantleEffect::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (IsEditorWorld())
	{
		RefreshEditorPreview();
	}
}

void AMantleEffect::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// The preview's own component is made on demand; one already there is left as it is.
	if (IsEditorWorld() && !EffectComponent)
	{
		RefreshEditorPreview();
	}
}

void AMantleEffect::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BurstTimer);
	}
	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void AMantleEffect::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);
	ArchetypeBeforeEdit = Archetype;
}

void AMantleEffect::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// A new archetype brings its own look, except where the creator already changed a value.
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(AMantleEffect, Archetype) && Archetype != ArchetypeBeforeEdit)
	{
		const FMantleEffectLook Old = FMantleEffects::DefaultLook(ArchetypeBeforeEdit);
		const FMantleEffectLook New = FMantleEffects::DefaultLook(Archetype);
		if (Color.Equals(Old.Color)) { Color = New.Color; }
		if (Size == Old.Size) { Size = New.Size; }
		if (Rate == Old.Rate) { Rate = New.Rate; }
		if (Speed == Old.Speed) { Speed = New.Speed; }
		if (Lifetime == Old.Lifetime) { Lifetime = New.Lifetime; }
	}
	ArchetypeBeforeEdit = Archetype;
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RefreshEditorPreview();
}
#endif

bool AMantleEffect::EnsureEffectComponent()
{
	if (EffectComponent && ComponentArchetype == Archetype)
	{
		return true;
	}
	DestroyEffectComponent();
	if (!DrawsEffects(GetWorld()))
	{
		return false;
	}
	UClass* ComponentClass = FMantleEffects::ComponentClass();
	UFXSystemAsset* System = ComponentClass ? FMantleEffects::LoadSystem(Archetype) : nullptr;
	if (!System)
	{
		return false;
	}
	UFXSystemComponent* Component = NewObject<UFXSystemComponent>(this, ComponentClass, NAME_None,
		RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
	if (!Component)
	{
		return false;
	}
	Component->SetAutoActivate(false);
	// In a level being edited it is the preview: never published, never saved, never cooked.
	Component->bIsEditorOnly = IsEditorWorld();
	if (!MantleEffectActorPrivate::SetEffectComponentSystem(*Component, *System))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_EFFECT %s has no SetAsset taking %s: effects draw nothing"),
			*ComponentClass->GetName(), *System->GetClass()->GetName());
		Component->MarkAsGarbage();
		return false;
	}
	Component->SetupAttachment(EffectRoot);
	Component->RegisterComponent();
	EffectComponent = Component;
	ComponentArchetype = Archetype;
	return true;
}

void AMantleEffect::DestroyEffectComponent()
{
	if (EffectComponent)
	{
		EffectComponent->DestroyComponent();
		EffectComponent = nullptr;
	}
	SpritesMaterial = nullptr;
}

void AMantleEffect::PushLook()
{
	if (!EffectComponent)
	{
		return;
	}
	if (Archetype == EMantleEffectArchetype::Sprites)
	{
		// #628: the sprites system reads its own keys; its material instance is made once per blend and updated after.
		const FMantleEffectLook Look = FMantleEffects::ClampLook(GetLook(), Archetype);
		const FMantleSpritesSettings Settings = FMantleEffects::ClampSprites(Sprites);
		if (!SpritesMaterial || SpritesMaterialBlend != Settings.Blend)
		{
			SpritesMaterial = MakeSpritesMaterial(this, Look, Settings, SpritesTexture);
			SpritesMaterialBlend = Settings.Blend;
		}
		else
		{
			MantleEffectActorPrivate::WriteSpritesMaterialValues(*SpritesMaterial, Look, Settings, SpritesTexture);
		}
		WriteSpritesParameters(*EffectComponent, Look, Settings, SpritesMaterial);
		return;
	}
	if (!WriteLookParameters(*EffectComponent, FMantleEffects::ClampLook(GetLook(), Archetype)))
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_EFFECT %s has no SetVariableVec2: effects keep their system's own size"),
				*EffectComponent->GetClass()->GetName());
		}
	}
}

bool AMantleEffect::WriteLookParameters(UFXSystemComponent& Component, const FMantleEffectLook& Look)
{
	Component.SetColorParameter(FName(FMantleEffects::ColorParameter), Look.Color);
	Component.SetFloatParameter(FName(FMantleEffects::RateParameter), static_cast<float>(Look.Rate));
	Component.SetFloatParameter(FName(FMantleEffects::SpeedParameter), static_cast<float>(Look.Speed));
	Component.SetFloatParameter(FName(FMantleEffects::LifetimeParameter), static_cast<float>(Look.Lifetime));
	// A lightweight emitter's Sprite Size binds only a Vector2D; the sprites are square.
	return MantleEffectActorPrivate::SetEffectComponentVec2(Component, FName(FMantleEffects::SizeParameter), FVector2D(Look.Size, Look.Size));
}

bool AMantleEffect::WriteSpritesParameters(UFXSystemComponent& Component, const FMantleEffectLook& Look, const FMantleSpritesSettings& InSprites,
	UMaterialInterface* Material)
{
	const FMantleSpritesSettings S = FMantleEffects::ClampSprites(InSprites);
	Component.SetFloatParameter(FName(FMantleEffects::RateParameter), static_cast<float>(FMantleEffects::SpritesEffectiveRate(Look.Rate, S)));
	Component.SetFloatParameter(FName(FMantleEffects::SizeMinParameter), static_cast<float>(S.SizeMin));
	Component.SetFloatParameter(FName(FMantleEffects::SizeMaxParameter), static_cast<float>(S.SizeMax));
	Component.SetFloatParameter(FName(FMantleEffects::LifetimeMinParameter), static_cast<float>(S.LifetimeMin));
	Component.SetFloatParameter(FName(FMantleEffects::LifetimeMaxParameter), static_cast<float>(S.LifetimeMax));
	Component.SetVectorParameter(FName(FMantleEffects::VelocityMinParameter), S.VelocityMin);
	Component.SetVectorParameter(FName(FMantleEffects::VelocityMaxParameter), S.VelocityMax);
	Component.SetVectorParameter(FName(FMantleEffects::AccelerationParameter), S.Acceleration);
	// A point is the box emitter with no extent; the other two volumes have their own emitters.
	const bool bBox = S.Shape == EMantleSpritesShape::Box || S.Shape == EMantleSpritesShape::Point;
	Component.SetVectorParameter(FName(FMantleEffects::BoxSizeParameter), S.Shape == EMantleSpritesShape::Box ? S.BoxHalfExtent * 2.0 : FVector::ZeroVector);
	Component.SetFloatParameter(FName(FMantleEffects::SphereRadiusParameter), static_cast<float>(S.SphereRadius));
	Component.SetFloatParameter(FName(FMantleEffects::CylinderRadiusParameter), static_cast<float>(S.CylinderRadius));
	Component.SetFloatParameter(FName(FMantleEffects::CylinderHeightParameter), static_cast<float>(S.CylinderHeight));
	Component.SetEmitterEnable(FName(FMantleEffects::BoxEmitterName), bBox);
	Component.SetEmitterEnable(FName(FMantleEffects::SphereEmitterName), S.Shape == EMantleSpritesShape::Sphere);
	Component.SetEmitterEnable(FName(FMantleEffects::CylinderEmitterName), S.Shape == EMantleSpritesShape::Cylinder);
	return !Material || MantleEffectActorPrivate::SetEffectComponentMaterial(Component, FName(FMantleEffects::MaterialParameter), Material);
}

UMaterialInstanceDynamic* AMantleEffect::MakeSpritesMaterial(UObject* Outer, const FMantleEffectLook& Look, const FMantleSpritesSettings& Sprites, UTexture* Texture)
{
	static bool bWarned[2] = { false, false };
	const FString Path = FMantleEffects::SpritesMaterialPath(Sprites.Blend);
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Parent)
	{
		bool& bAlreadyWarned = bWarned[Sprites.Blend == EMantleSpritesBlend::Additive ? 1 : 0];
		if (!bAlreadyWarned)
		{
			bAlreadyWarned = true;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_EFFECT %s is missing from this build (Create-MantleEffectSprites.py makes it): sprites effects draw with the system's own material"), *Path);
		}
		return nullptr;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Outer);
	if (Instance)
	{
		MantleEffectActorPrivate::WriteSpritesMaterialValues(*Instance, Look, FMantleEffects::ClampSprites(Sprites), Texture);
	}
	return Instance;
}

void AMantleEffect::FinishBurst()
{
	bPlaying = false;
	if (EffectComponent)
	{
		EffectComponent->Deactivate();
	}
}

void AMantleEffect::ScheduleBurstEnd(const double Seconds)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (Seconds <= 0.0)
	{
		World->GetTimerManager().ClearTimer(BurstTimer);
		FinishBurst();
		return;
	}
	World->GetTimerManager().SetTimer(BurstTimer, this, &AMantleEffect::FinishBurst, static_cast<float>(Seconds), false);
}

void AMantleEffect::RefreshEditorPreview()
{
	if (!IsEditorWorld() || IsRunningCommandlet())
	{
		return;
	}
	if (!EnsureEffectComponent())
	{
		return;
	}
	PushLook();
	// The preview always loops, so the look can be judged while editing it.
	if (!EffectComponent->IsActive())
	{
		EffectComponent->Activate(/*bReset*/ true);
		if (Archetype == EMantleEffectArchetype::Sprites)
		{
			// The volume emitters are switched on a running system: say which again once it is active.
			PushLook();
		}
	}
}
