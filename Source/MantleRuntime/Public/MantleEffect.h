// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/Actor.h"
#include "MantleEffectTypes.h"
#include "MantlePlaceTypes.h"

#include "MantleEffect.generated.h"

class UBillboardComponent;
class UFXSystemAsset;
class UFXSystemComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture;
struct FPropertyChangedEvent;

/**
 * A particle effect (#460): the Effect place entity. A creator places one and picks a built-in archetype
 * and its look; the place builder spawns one per Effect entity; a script's Effects:Play spawns a short-lived
 * one. Never replicated: a server script's changes reach clients through the script appearance map
 * (Entity:PlayEffect, StopEffect, SetEffectColor, SetEffectRate), and its one-shots through the script net
 * component.
 *
 * Only a machine that draws shows anything: the archetype's Niagara system (FMantleEffects::LoadSystem)
 * in a component made at runtime (FMantleEffects::ComponentClass). A dedicated server, a build without the
 * archetype's asset, or one without Niagara keeps the actor and its state and draws nothing. In the editor
 * the actor previews its effect, looping, whenever the archetype's asset exists.
 *
 * The particles leave along the actor's up (+Z) axis.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Effect"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision, Rendering))
class MANTLERUNTIME_API AMantleEffect : public AActor
{
	GENERATED_BODY()

public:
	AMantleEffect();

	/** Which built-in effect. Changing it resets the values below that still hold the old one's own look. */
	UPROPERTY(EditAnywhere, Category = "Effect")
	EMantleEffectArchetype Archetype = EMantleEffectArchetype::Smoke;

	/** The particles' tint, linear RGB (alpha is ignored). Fire recolours its flames to it; white keeps their own colours. */
	UPROPERTY(EditAnywhere, Category = "Effect", meta = (HideAlphaChannel))
	FLinearColor Color;

	/** Particle size, 1 to 1000 cm. */
	UPROPERTY(EditAnywhere, Category = "Effect", meta = (ClampMin = "1", ClampMax = "1000", UIMin = "1", UIMax = "1000", Units = "cm"))
	double Size;

	/** Particles spawned a second, 0 to 200. */
	UPROPERTY(EditAnywhere, Category = "Effect", meta = (ClampMin = "0", ClampMax = "200", UIMin = "0", UIMax = "200"))
	double Rate;

	/** How fast particles leave the effect, 0 to 5000 cm/s. */
	UPROPERTY(EditAnywhere, Category = "Effect", meta = (ClampMin = "0", ClampMax = "5000", UIMin = "0", UIMax = "5000", Units = "cm/s"))
	double Speed;

	/** How long each particle lives, 0.05 to 10 seconds. */
	UPROPERTY(EditAnywhere, Category = "Effect", meta = (ClampMin = "0.05", ClampMax = "10", UIMin = "0.05", UIMax = "10", Units = "s"))
	double Lifetime;

	/** Start when a player joins. Off waits for a script's Entity:PlayEffect. */
	UPROPERTY(EditAnywhere, Category = "Effect")
	bool bAutoplay = true;

	/** Emit for as long as it plays. Off emits for one second each time it starts, then lets its particles die out. */
	UPROPERTY(EditAnywhere, Category = "Effect")
	bool bLoop = true;

	/** The sprites archetype's keys (#628): read only when Archetype is Sprites. */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (EditCondition = "Archetype == EMantleEffectArchetype::Sprites", EditConditionHides))
	FMantleSpritesSettings Sprites;

	/**
	 * The sprites archetype's texture (#628), drawn on every particle; empty draws a soft round dot. In the editor a
	 * creator picks it; in a built place the place builder assigns the downloaded one.
	 */
	UPROPERTY(EditAnywhere, Category = "Sprites", meta = (EditCondition = "Archetype == EMantleEffectArchetype::Sprites", EditConditionHides))
	TObjectPtr<UTexture> SpritesTexture;

	/** Sets the actor from Effect properties: the place builder's and the editor importer's. */
	void ApplyProps(const FMantleEffectProps& In);

	/**
	 * Reads the actor into Effect properties. False, with OutProblem phrased to follow the actor's name and
	 * saying what to fix, when a value is outside the schema's limits. Out is filled either way.
	 */
	bool ReadProps(FMantleEffectProps& Out, FString& OutProblem) const;

	/** The look the actor holds now, as the effect's user parameters get it. */
	FMantleEffectLook GetLook() const;

	/**
	 * The place builder's last step: in a game world, an autoplaying effect starts. In an editor world (an
	 * import into a level) the preview follows the values instead.
	 */
	void BeginPlaceEffect();

	/**
	 * Starts emitting (a looping effect already playing keeps going; a non-looping one starts a new burst of
	 * FMantleEffects::OneShotEmitSeconds). Every machine keeps the state; only one that draws shows it.
	 */
	void PlayEffect();

	/** Stops emitting; the particles already out die out. */
	void StopEffect();

	/** Retints the particles (clamped to 0..1); the next particles take it. */
	void SetEffectColor(const FLinearColor& InColor);

	/** Changes how many particles spawn a second (clamped to 0..FMantlePlaceLimits::MaxEffectRate). */
	void SetEffectRate(double InRate);

	/** Whether the effect is emitting: after PlayEffect (or autoplay) until StopEffect or a one-shot's end. */
	bool IsEffectPlaying() const { return bPlaying; }

	/** The component drawing the effect, or null where nothing draws (a server, a missing asset, no Niagara). */
	UFXSystemComponent* GetEffectComponent() const;

	/**
	 * Spawns a short-lived, transient effect at Transform (Effects:Play): it emits for EmitSeconds, then
	 * destroys itself once its last particles are gone. Null where nothing would draw: a dedicated server,
	 * or a build without the archetype's asset or Niagara (each logged once).
	 */
	static AMantleEffect* SpawnOneShot(UWorld* World, const FTransform& Transform, EMantleEffectArchetype InArchetype,
		const FMantleEffectLook& InLook, double EmitSeconds);

	/**
	 * Spawns a short-lived, transient actor drawing System, any Niagara system (not one of the archetypes: a script's
	 * Effects:Beam uses the weapon beams'), at Transform for LifeSeconds, then destroying itself. Returns its component,
	 * not yet active: the caller sets user parameters on it (SetSystemVariableVec3 and the UFXSystemComponent setters),
	 * then calls Activate(true); the owner is the actor. Null where nothing would draw (a dedicated server, no Niagara, no System).
	 */
	static UFXSystemComponent* SpawnSystem(UWorld* World, const FTransform& Transform, UFXSystemAsset* System, double LifeSeconds);

	/**
	 * Writes Value to Component's Vector3 or position user parameter Name through Niagara's UFUNCTION FunctionName
	 * ("SetVariableVec3" or "SetVariablePosition"), by reflection like the setters above. False when the class has no such function.
	 */
	static bool SetSystemVariableVec3(UFXSystemComponent& Component, const TCHAR* FunctionName, const FName Name, const FVector& Value);

	/**
	 * Hands Look, as given, to Component's user parameters (FMantleEffects' names and types): the colour a
	 * LinearColor, the size a Vector2D (Size, Size), the rate, speed and lifetime floats. The Vector2D goes
	 * through Niagara's SetVariableVec2 by reflection, as UFXSystemComponent has no such setter. False when
	 * Component has none (it is not Niagara's): the size is then unwritten, the rest written anyway.
	 */
	static bool WriteLookParameters(UFXSystemComponent& Component, const FMantleEffectLook& Look);

	/**
	 * Hands a sprites effect's keys to Component's user parameters (FMantleEffects' sprites names): the rate (lowered to
	 * keep within MaxParticles), the size and lifetime ranges, the velocity range and acceleration, the shape's extents,
	 * and the material instance Material (the renderer's User.Material). The volume emitter for the shape is enabled and
	 * the others disabled through SetEmitterEnable. Settings are held to the schema's limits first. A parameter the system
	 * does not have is ignored by Niagara; the Material goes through SetVariableMaterial by reflection (false when the
	 * component has none).
	 */
	static bool WriteSpritesParameters(UFXSystemComponent& Component, const FMantleEffectLook& Look, const FMantleSpritesSettings& Sprites,
		UMaterialInterface* Material);

	/**
	 * The material instance a sprites effect draws with: the translucent or additive master (FMantleEffects::SpritesMaterialPath),
	 * with the texture (Texture, or the soft dot when null), colours, opacity envelope and end scale set. Null where the master
	 * is missing from this build (logged once).
	 */
	static UMaterialInstanceDynamic* MakeSpritesMaterial(UObject* Outer, const FMantleEffectLook& Look, const FMantleSpritesSettings& Sprites, UTexture* Texture);

	/** Whether World draws effects: not a dedicated server. */
	static bool DrawsEffects(const UWorld* World);

	/**
	 * How many times a server script's Entity:PlayEffect has started this effect, as far as this machine has
	 * applied (the script appearance map's EffectPlays): a re-sent map with the same count starts nothing.
	 */
	int32 ScriptPlayCount = 0;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	/** A level opened in the editor registers its actors without running their construction: the preview starts here then. */
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Makes the component for the current archetype (replacing one of another archetype); false where nothing draws. */
	bool EnsureEffectComponent();
	void DestroyEffectComponent();
	/** Hands the look to the component's user parameters. */
	void PushLook();
	/** Ends a non-looping effect's burst. */
	void FinishBurst();
	/** Ends the burst Seconds from now (a world that never ticks keeps it playing). */
	void ScheduleBurstEnd(double Seconds);
	/** The editor preview: the effect, looping, in a level being edited. */
	void RefreshEditorPreview();
	bool IsEditorWorld() const;

	UPROPERTY(VisibleAnywhere, Category = "Effect")
	TObjectPtr<USceneComponent> EffectRoot;

	/** Made at runtime (and for the editor preview), never saved. */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	TObjectPtr<UFXSystemComponent> EffectComponent;

	/** The archetype EffectComponent draws. */
	EMantleEffectArchetype ComponentArchetype = EMantleEffectArchetype::Smoke;

	/** A sprites effect's material instance (#628), made at runtime and remade when its blend changes; never saved. */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	TObjectPtr<UMaterialInstanceDynamic> SpritesMaterial;

	/** The blend SpritesMaterial was made for. */
	EMantleSpritesBlend SpritesMaterialBlend = EMantleSpritesBlend::Translucent;

	bool bPlaying = false;
	FTimerHandle BurstTimer;

#if WITH_EDITOR
	/** The archetype before an edit, for PostEditChangeProperty's default swap. */
	EMantleEffectArchetype ArchetypeBeforeEdit = EMantleEffectArchetype::Smoke;
#endif

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> EditorBillboard;
#endif
};
