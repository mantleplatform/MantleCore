// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"
#include "Subsystems/WorldSubsystem.h"

#include "MantleReflectionProbe.generated.h"

class UReflectionCaptureComponent;
class USceneComponent;

/**
 * What the engine needs for a runtime-captured reflection probe (#628), and the numbers the pump works to.
 *
 * A classic Box or Sphere Reflection Capture is baked in the editor and only uploads its MapBuildData in a game
 * (ReflectionEnvironmentCapture.cpp: "Capturing only supported in the editor"), and a place has no baked data, so
 * a probe is captured on each client instead: UReflectionCaptureComponent::bRuntimeCapture, with the project's
 * r.ReflectionCapture.Runtime=1 (DefaultEngine.ini) which makes the renderer give a runtime capture its proxy.
 * The renderer renders one capture a frame, one face (r.ReflectionCapture.Runtime.Timeslice) at a time, but a
 * packaged game only runs that update on a frame where a capture is queued (UGameEngine::Tick asks
 * UReflectionCaptureComponent::HasReflectionCapturesToUpdate first), so UMantleReflectionProbeSubsystem queues
 * one every frame until every probe has had its turn.
 *
 * A runtime capture's average brightness is never measured (only the baked path runs ComputeAverageBrightness), so
 * it stays 0, and the renderer's lightmap mixing divides a rough surface's reflection by it (ComputeMixingWeight):
 * hence r.ReflectionEnvironmentLightmapMixing=0 in DefaultEngine.ini, and IsLightmapMixingOn for the log.
 */
struct MANTLERUNTIME_API FMantleReflectionProbes
{
	/**
	 * Frames the subsystem waits after the place is built before it creates the captures: the real-time sky light
	 * (FMantleEnvironment sets SetRealTimeCapture) needs a few frames to capture the sky and light the interiors
	 * the probes will see.
	 */
	static constexpr int32 WarmupFrames = 30;

	/** Frames of slack past the last probe's own, for the allocation update and a frame that renders nothing. */
	static constexpr int32 PumpSlackFrames = 4;

	/**
	 * The pump also runs at least this many seconds. The engine skips its capture update on every frame the shader compiler
	 * has work (UpdateReflectionCaptureContents), which an uncooked run can have for a while, and a frame count alone would
	 * run out inside it. The extra frames cost one queued capture each and render nothing (a finished capture is skipped).
	 */
	static constexpr double MinPumpSeconds = 5.0;

	/** A cubemap face count, which every capture renders. */
	static constexpr int32 CubeFaces = 6;

	/** "Mantle.ReflectionProbes" is not 0 (an A/B switch for looking at a place with and without its probes). */
	static bool IsEnabled();

	/** r.ReflectionCapture.Runtime is not 0: the renderer only gives a runtime capture a proxy then. */
	static bool IsRuntimeCaptureOn();

	/** r.ReflectionEnvironmentLightmapMixing is not 0: a runtime capture then lights rough surfaces far too brightly. */
	static bool IsLightmapMixingOn();

	/** Empty when World draws probes; otherwise the reason it does not (a dedicated server, a process that does not render, a cvar off), for the log. */
	static FString WhyNotDrawn(const UWorld* World);

	/**
	 * Frames to queue a capture for ProbeCount probes: each takes ceil(6 / TimesliceFaces) updates, plus one (the
	 * shadows-only first slice of a one-face timeslice), twice as long when TimesliceSlow halves the rate, plus
	 * PumpSlackFrames. 0 for no probes. TimesliceFaces is r.ReflectionCapture.Runtime.Timeslice, clamped to 1..6.
	 */
	static int32 PumpFrames(int32 ProbeCount, int32 TimesliceFaces, bool bTimesliceSlow);

	/** PumpFrames for the engine's present cvars. */
	static int32 PumpFramesNow(int32 ProbeCount);
};

/**
 * One ReflectionProbe entity as an actor (#628). It holds the probe's numbers and stands where the place puts the
 * entity; the capture component is not made until UMantleReflectionProbeSubsystem asks (SpawnCapture), once the
 * place is built, so a dedicated server or a process that cannot render keeps the entity and creates nothing.
 *
 * The capture is made on this actor, which is why the actor is a plain scene root: the renderer takes the capture
 * position from the owner's location (RuntimeCaptureActor->SetActorLocation(Owner->GetActorLocation())), and the
 * capture component is a Static one, which the engine will not attach to a Movable root, so it stays unattached
 * and takes its world transform from the actor's at the moment it is made.
 */
UCLASS(NotPlaceable)
class MANTLERUNTIME_API AMantleReflectionProbe : public AActor
{
	GENERATED_BODY()

public:
	AMantleReflectionProbe();

	void SetProbeProps(const FMantleReflectionProbeProps& InProps) { ProbeProps = InProps; }
	const FMantleReflectionProbeProps& GetProbeProps() const { return ProbeProps; }

	/**
	 * Makes and registers the runtime capture component for ProbeProps at this actor's present world transform
	 * (its centre and rotation; the scale of the box is the half extent, whatever the actor's scale is). True when
	 * it made one; false when it already has, or the component could not be made. Does not look at the cvars:
	 * UMantleReflectionProbeSubsystem decides whether probes are drawn at all.
	 */
	bool SpawnCapture();

	/** The capture component SpawnCapture made, or null. */
	UReflectionCaptureComponent* GetCapture() const { return Capture; }

	/** The world transform a capture for Props gets when the probe stands at ActorTransform: its location and rotation, and a box's half extent as the scale. */
	static FTransform CaptureTransform(const FMantleReflectionProbeProps& Props, const FTransform& ActorTransform);

	/** Sets Component up for Props before it registers: the runtime flag, brightness, the sky light leak, and the shape's own size. */
	static void ConfigureCapture(UReflectionCaptureComponent* Component, const FMantleReflectionProbeProps& Props);

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(Transient)
	TObjectPtr<UReflectionCaptureComponent> Capture;

	FMantleReflectionProbeProps ProbeProps;
};

/**
 * Captures a place's probes once it stands (#628). The builder registers each probe as it spawns it and tells
 * the subsystem when the build is over; after FMantleReflectionProbes::WarmupFrames the subsystem makes every
 * capture component, then queues one capture a frame (UReflectionCaptureComponent::MarkDirtyForRecaptureOrUpload)
 * for FMantleReflectionProbes::PumpFrames frames (and at least FMantleReflectionProbes::MinPumpSeconds), which is what
 * keeps the engine rendering the cube faces.
 *
 * Logs `MANTLE_PLACE_REFLECTION spawned=N` when the captures are made and
 * `MANTLE_PLACE_REFLECTION captured=N frames=F` when the last frame has been queued, or
 * `MANTLE_PLACE_REFLECTION skipped=N reason=...` when a place has probes and this process draws none.
 * The renderer gives no signal when a face has been rendered, so "captured" means every capture had its turns
 * queued; the proof that they rendered is the picture.
 */
UCLASS()
class MANTLERUNTIME_API UMantleReflectionProbeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UMantleReflectionProbeSubsystem, STATGROUP_Tickables);
	}
	virtual void Tick(float DeltaTime) override;

	/** The world's subsystem, or null where none is made (a dedicated server, an editor world). */
	static UMantleReflectionProbeSubsystem* Get(UWorld* World);

	/**
	 * Called by FMantlePlaceBuilder::Build at its end: the place stands, so the warm-up starts for the probes
	 * registered since. Logs the skip line when Place has probes and World draws none.
	 */
	static void NotifyPlaceBuilt(UWorld* World, const FMantlePlace& Place);

	/** A built probe, to be captured after the next NotifyPlaceBuilt. */
	void Register(AMantleReflectionProbe* Probe);

	/** Probes registered and not yet captured. */
	int32 NumPending() const { return Pending.Num(); }

private:
	enum class EPhase : uint8
	{
		Idle,
		Warmup,
		Pumping
	};

	void BeginWarmup();
	void StartCapture();
	void PumpOnce();

	TArray<TWeakObjectPtr<AMantleReflectionProbe>> Pending;
	TArray<TWeakObjectPtr<UReflectionCaptureComponent>> Captures;
	EPhase Phase = EPhase::Idle;
	int32 WarmupLeft = 0;
	int32 FramesLeft = 0;
	int32 FramesPumped = 0;
	int32 NextCapture = 0;
	double PumpStartSeconds = 0.0;
};
