// Copyright Joshua Eger. All Rights Reserved.
#include "MantleReflectionProbe.h"
#include "MantleRuntime.h"

#include "Components/BoxReflectionCaptureComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleReflectionProbePrivate
{
	TAutoConsoleVariable<int32> CVarProbes(
		TEXT("Mantle.ReflectionProbes"),
		1,
		TEXT("Capture a place's reflection probes at run time (1, the default since their brightness matches the editor, #722) or leave them out (0). Read when a place is built.\n"),
		ECVF_Default);

	/** An int cvar of the engine's by name, or Fallback when this build does not have it. */
	int32 ProbeEngineInt(const TCHAR* Name, int32 Fallback)
	{
		const TConsoleVariableData<int32>* Variable = IConsoleManager::Get().FindTConsoleVariableDataInt(Name);
		return Variable ? Variable->GetValueOnGameThread() : Fallback;
	}
}

// ---- FMantleReflectionProbes -----------------------------------------------------------------------

bool FMantleReflectionProbes::IsEnabled()
{
	return MantleReflectionProbePrivate::CVarProbes.GetValueOnGameThread() != 0;
}

bool FMantleReflectionProbes::IsRuntimeCaptureOn()
{
	return MantleReflectionProbePrivate::ProbeEngineInt(TEXT("r.ReflectionCapture.Runtime"), 0) != 0;
}

bool FMantleReflectionProbes::IsLightmapMixingOn()
{
	return MantleReflectionProbePrivate::ProbeEngineInt(TEXT("r.ReflectionEnvironmentLightmapMixing"), 0) != 0;
}

FString FMantleReflectionProbes::WhyNotDrawn(const UWorld* World)
{
	if (!World || !World->IsGameWorld())
	{
		return TEXT("this is not a game world");
	}
	if (IsRunningDedicatedServer() || World->GetNetMode() == NM_DedicatedServer)
	{
		return TEXT("a dedicated server draws nothing");
	}
	if (!FApp::CanEverRender())
	{
		return TEXT("this process does not render");
	}
	if (!IsEnabled())
	{
		return TEXT("Mantle.ReflectionProbes is 0");
	}
	if (!IsRuntimeCaptureOn())
	{
		return TEXT("r.ReflectionCapture.Runtime is 0");
	}
	return FString();
}

int32 FMantleReflectionProbes::PumpFrames(int32 ProbeCount, int32 TimesliceFaces, bool bTimesliceSlow)
{
	if (ProbeCount <= 0)
	{
		return 0;
	}
	const int32 Faces = FMath::Clamp(TimesliceFaces, 1, CubeFaces);
	const int32 UpdatesPerProbe = FMath::DivideAndRoundUp(CubeFaces, Faces) + 1;
	// The engine's slow mode renders every other frame, and only when the timeslice is one face.
	const int32 Rate = bTimesliceSlow && Faces == 1 ? 2 : 1;
	return ProbeCount * UpdatesPerProbe * Rate + PumpSlackFrames;
}

int32 FMantleReflectionProbes::PumpFramesNow(int32 ProbeCount)
{
	using namespace MantleReflectionProbePrivate;
	return PumpFrames(ProbeCount, ProbeEngineInt(TEXT("r.ReflectionCapture.Runtime.Timeslice"), 1),
		ProbeEngineInt(TEXT("r.ReflectionCapture.Runtime.TimesliceSlow"), 0) != 0);
}

// ---- AMantleReflectionProbe ------------------------------------------------------------------------

AMantleReflectionProbe::AMantleReflectionProbe()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

FTransform AMantleReflectionProbe::CaptureTransform(const FMantleReflectionProbeProps& Props, const FTransform& ActorTransform)
{
	const FVector Scale = Props.Shape == EMantleReflectionProbeShape::Box ? Props.HalfExtent : FVector::OneVector;
	return FTransform(ActorTransform.GetRotation(), ActorTransform.GetLocation(), Scale);
}

void AMantleReflectionProbe::ConfigureCapture(UReflectionCaptureComponent* Component, const FMantleReflectionProbeProps& Props)
{
	if (!Component)
	{
		return;
	}
	// Before the component registers: the renderer decides at registration whether this capture gets a proxy.
	Component->bRuntimeCapture = true;
	Component->Brightness = static_cast<float>(Props.Brightness);
	const float Leak = static_cast<float>(Props.SkylightScale);
	Component->RuntimeSkylightScale = FLinearColor(Leak, Leak, Leak);
	if (UBoxReflectionCaptureComponent* Box = Cast<UBoxReflectionCaptureComponent>(Component))
	{
		Box->BoxTransitionDistance = static_cast<float>(Props.TransitionDistance);
	}
	else if (USphereReflectionCaptureComponent* Sphere = Cast<USphereReflectionCaptureComponent>(Component))
	{
		Sphere->InfluenceRadius = static_cast<float>(Props.InfluenceRadius);
	}
}

bool AMantleReflectionProbe::SpawnCapture()
{
	if (Capture)
	{
		return false;
	}
	UReflectionCaptureComponent* Component = nullptr;
	if (ProbeProps.Shape == EMantleReflectionProbeShape::Box)
	{
		Component = NewObject<UBoxReflectionCaptureComponent>(this, TEXT("Capture"), RF_Transient);
	}
	else
	{
		Component = NewObject<USphereReflectionCaptureComponent>(this, TEXT("Capture"), RF_Transient);
	}
	if (!Component)
	{
		return false;
	}
	ConfigureCapture(Component, ProbeProps);

	// Unattached, so its transform is the world's: set before it registers (it is Static), never moved after.
	const FTransform Transform = CaptureTransform(ProbeProps, GetActorTransform());
	Component->SetRelativeLocation_Direct(Transform.GetLocation());
	Component->SetRelativeRotation_Direct(Transform.Rotator());
	Component->SetRelativeScale3D_Direct(Transform.GetScale3D());

	// Listed as the actor's own, then registered: the proxy is made now.
	AddInstanceComponent(Component);
	Component->RegisterComponent();
	Capture = Component;
	return true;
}

// ---- UMantleReflectionProbeSubsystem ---------------------------------------------------------------

bool UMantleReflectionProbeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return !IsRunningDedicatedServer() && !IsRunningCommandlet() && Super::ShouldCreateSubsystem(Outer);
}

bool UMantleReflectionProbeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// A creator's editor level (the importer's) holds no captures of ours; PIE and the game do.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UMantleReflectionProbeSubsystem::Deinitialize()
{
	Pending.Reset();
	Captures.Reset();
	Phase = EPhase::Idle;
	Super::Deinitialize();
}

UMantleReflectionProbeSubsystem* UMantleReflectionProbeSubsystem::Get(UWorld* World)
{
	return World ? World->GetSubsystem<UMantleReflectionProbeSubsystem>() : nullptr;
}

void UMantleReflectionProbeSubsystem::NotifyPlaceBuilt(UWorld* World, const FMantlePlace& Place)
{
	int32 Probes = 0;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		Probes += Entity.Type == EMantleEntityType::ReflectionProbe ? 1 : 0;
	}
	if (Probes == 0)
	{
		return;
	}
	FString Why = FMantleReflectionProbes::WhyNotDrawn(World);
	UMantleReflectionProbeSubsystem* Subsystem = Get(World);
	if (Why.IsEmpty() && !Subsystem)
	{
		Why = TEXT("this world has no reflection probe subsystem");
	}
	if (!Why.IsEmpty())
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION skipped=%d reason=%s"), Probes, *Why);
		return;
	}
	if (Subsystem->NumPending() > 0)
	{
		Subsystem->BeginWarmup();
	}
}

void UMantleReflectionProbeSubsystem::Register(AMantleReflectionProbe* Probe)
{
	if (Probe)
	{
		Pending.AddUnique(Probe);
	}
}

void UMantleReflectionProbeSubsystem::BeginWarmup()
{
	// A build that ends while an earlier one is still being captured waits for it: its probes stay pending.
	if (Phase == EPhase::Pumping)
	{
		return;
	}
	Phase = EPhase::Warmup;
	WarmupLeft = FMantleReflectionProbes::WarmupFrames;
}

void UMantleReflectionProbeSubsystem::Tick(float DeltaTime)
{
	if (Phase == EPhase::Idle)
	{
		return;
	}
	if (Phase == EPhase::Warmup)
	{
		if (--WarmupLeft <= 0)
		{
			StartCapture();
		}
		return;
	}
	PumpOnce();
}

void UMantleReflectionProbeSubsystem::StartCapture()
{
	Captures.Reset();
	for (const TWeakObjectPtr<AMantleReflectionProbe>& Pointer : Pending)
	{
		AMantleReflectionProbe* Probe = Pointer.Get();
		if (IsValid(Probe) && Probe->SpawnCapture())
		{
			Captures.Add(Probe->GetCapture());
		}
	}
	Pending.Reset();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION spawned=%d"), Captures.Num());
	if (!Captures.IsEmpty() && FMantleReflectionProbes::IsLightmapMixingOn())
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_REFLECTION r.ReflectionEnvironmentLightmapMixing is on: runtime captures have no average brightness, so rough surfaces will read far too bright"));
	}
	if (Captures.IsEmpty())
	{
		Phase = EPhase::Idle;
		return;
	}
	// Every capture queued once, so the first update allocates all of them; then one a frame (PumpOnce).
	for (const TWeakObjectPtr<UReflectionCaptureComponent>& Pointer : Captures)
	{
		if (UReflectionCaptureComponent* Capture = Pointer.Get())
		{
			Capture->MarkDirtyForRecaptureOrUpload();
		}
	}
	FramesLeft = FMantleReflectionProbes::PumpFramesNow(Captures.Num());
	FramesPumped = 0;
	NextCapture = 0;
	PumpStartSeconds = FPlatformTime::Seconds();
	Phase = EPhase::Pumping;
}

void UMantleReflectionProbeSubsystem::PumpOnce()
{
	// One queued capture is enough to make the engine run its capture update this frame, and each update
	// renders the next face of the nearest capture that is not finished (Once mode skips the finished ones).
	int32 Valid = 0;
	bool bQueued = false;
	for (int32 Step = 0; Step < Captures.Num(); ++Step)
	{
		UReflectionCaptureComponent* Capture = Captures[(NextCapture + Step) % Captures.Num()].Get();
		if (!IsValid(Capture))
		{
			continue;
		}
		++Valid;
		if (!bQueued)
		{
			Capture->MarkDirtyForRecaptureOrUpload();
			NextCapture = (NextCapture + Step + 1) % Captures.Num();
			bQueued = true;
		}
	}
	++FramesPumped;
	--FramesLeft;
	const bool bLongEnough = FPlatformTime::Seconds() - PumpStartSeconds >= FMantleReflectionProbes::MinPumpSeconds;
	if (Valid == 0 || (FramesLeft <= 0 && bLongEnough))
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION captured=%d frames=%d"), Valid, FramesPumped);
		Captures.Reset();
		Phase = EPhase::Idle;
		// Probes of a build that ended meanwhile.
		if (Pending.Num() > 0)
		{
			BeginWarmup();
		}
	}
}
