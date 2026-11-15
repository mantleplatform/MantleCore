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
		TEXT("Capture a place's reflection probes at run time (1, the default since their brightness matches the editor) or leave them out (0). Read when a place is built.\n"),
		ECVF_Default);

	/**
	 * An int cvar of the engine's by name, or Fallback when this build does not have it. Read through IConsoleVariable,
	 * not FindTConsoleVariableDataInt: some of these are FAutoConsoleVariableRefs, which have no TConsoleVariableData
	 * and ensure when asked for it (IConsoleManager.h:511).
	 */
	int32 ProbeEngineInt(const TCHAR* Name, int32 Fallback)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? Variable->GetInt() : Fallback;
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

double FMantleReflectionProbes::ReachCm(const FMantleReflectionProbeProps& Props)
{
	return Props.Shape == EMantleReflectionProbeShape::Sphere ? Props.InfluenceRadius : FMath::Max3(Props.HalfExtent.X, Props.HalfExtent.Y, Props.HalfExtent.Z);
}

bool FMantleReflectionProbes::WantsRecapture(const TSet<int32>& Seen, const FMantleCellAreaState& Now)
{
	if (!Now.ReadyToCapture())
	{
		return false;
	}
	for (const int32 CellIndex : Now.BuiltCellIds)
	{
		if (!Seen.Contains(CellIndex))
		{
			return true;
		}
	}
	return false;
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
	Live.Reset();
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
	if (Probe && !Pending.ContainsByPredicate([Probe](const FWaitingProbe& Waiting) { return Waiting.Probe.Get() == Probe; }))
	{
		FWaitingProbe Waiting;
		Waiting.Probe = Probe;
		Pending.Add(Waiting);
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

bool UMantleReflectionProbeSubsystem::AskArea(const AMantleReflectionProbe* Probe, FMantleCellAreaState& OutState) const
{
	FMantleCellAreaQuery Query;
	Query.Point = Probe->GetActorLocation();
	Query.RadiusCm = FMantleReflectionProbes::ReachCm(Probe->GetProbeProps());
	Query.bWantCellIds = true;
	uint64 Serial = 0;
	if (!FMantleCellArea::Ask(GetWorld(), Query, OutState, Serial))
	{
		// This world builds no cells (the place stands whole): nothing to wait for, and no cell that could come later.
		OutState = FMantleCellAreaState();
		return true;
	}
	return OutState.ReadyToCapture();
}

void UMantleReflectionProbeSubsystem::Tick(float DeltaTime)
{
	if (Phase == EPhase::Idle)
	{
		if ((Pending.Num() > 0 || Live.Num() > 0) && ++FramesSinceCheck >= FMantleReflectionProbes::ReadyCheckFrames)
		{
			FramesSinceCheck = 0;
			CheckAreas();
		}
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
	PumpOnce(DeltaTime);
}

void UMantleReflectionProbeSubsystem::StartCapture()
{
	// The place's build is over and the sky has had its frames. A probe whose surroundings stand is captured; one whose cells are still coming waits for them.
	Phase = EPhase::Idle;
	FramesSinceCheck = 0;
	CheckAreas();
}

void UMantleReflectionProbeSubsystem::CheckAreas()
{
	const double CheckStart = FPlatformTime::Seconds();
	const UWorld* World = GetWorld();
	const double WorldSeconds = World ? World->GetTimeSeconds() : 0.0;
	Captures.Reset();
	int32 First = 0;
	int32 Again = 0;
	double SpawnSeconds = 0.0;

	// Probes waiting for a first capture: made once their surroundings have been ready on two asks in a row.
	for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
	{
		FWaitingProbe& Waiting = Pending[Index];
		AMantleReflectionProbe* Probe = Waiting.Probe.Get();
		if (!IsValid(Probe))
		{
			Pending.RemoveAtSwap(Index);
			continue;
		}
		FMantleCellAreaState State;
		const bool bReady = AskArea(Probe, State);
		if (!bReady)
		{
			Waiting.bReadyBefore = false;
			if (!Waiting.bWaitingLogged)
			{
				Waiting.bWaitingLogged = true;
				UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION waiting probe=%s at=(%.0f, %.0f, %.0f) t=%.2f cells=%d built=%d pending=%d failed=%d plan_stale=%d"),
					*Probe->GetName(), Probe->GetActorLocation().X, Probe->GetActorLocation().Y, Probe->GetActorLocation().Z, WorldSeconds, State.Cells, State.Built, State.Pending, State.Failed, State.bPlanStale ? 1 : 0);
			}
			continue;
		}
		if (!Waiting.bReadyBefore)
		{
			Waiting.bReadyBefore = true;
			continue;
		}
		const double SpawnStart = FPlatformTime::Seconds();
		if (Probe->SpawnCapture())
		{
			const double SpawnTook = FPlatformTime::Seconds() - SpawnStart;
			SpawnSeconds += SpawnTook;
			Captures.Add(Probe->GetCapture());
			FLiveProbe Entry;
			Entry.Probe = Probe;
			Entry.SeenCells.Append(State.BuiltCellIds);
			Live.Add(Entry);
			++First;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION capture probe=%s at=(%.0f, %.0f, %.0f) reason=first t=%.2f cells=%d built=%d pending=%d spawn_ms=%.3f"),
				*Probe->GetName(), Probe->GetActorLocation().X, Probe->GetActorLocation().Y, Probe->GetActorLocation().Z, WorldSeconds, State.Cells, State.Built, State.Pending, SpawnTook * 1000.0);
		}
		Pending.RemoveAtSwap(Index);
	}

	// Probes that have a capture: again when a neighbour that changes what they see has been built.
	for (FLiveProbe& Entry : Live)
	{
		AMantleReflectionProbe* Probe = Entry.Probe.Get();
		UReflectionCaptureComponent* Capture = IsValid(Probe) ? Probe->GetCapture() : nullptr;
		if (!Capture || Captures.ContainsByPredicate([Capture](const TWeakObjectPtr<UReflectionCaptureComponent>& Held) { return Held.Get() == Capture; }))
		{
			continue;
		}
		FMantleCellAreaState State;
		AskArea(Probe, State);
		// Cells taken down change nothing: the picture keeps what it had, and the same cells coming back are what it already holds.
		if (!FMantleReflectionProbes::WantsRecapture(Entry.SeenCells, State))
		{
			Entry.bWantedBefore = false;
			continue;
		}
		if (!Entry.bWantedBefore)
		{
			Entry.bWantedBefore = true;
			continue;
		}
		int32 NewCells = 0;
		for (const int32 CellIndex : State.BuiltCellIds)
		{
			NewCells += Entry.SeenCells.Contains(CellIndex) ? 0 : 1;
		}
		const double RefreshStart = FPlatformTime::Seconds();
		Capture->RefreshCapture();
		const double RefreshTook = FPlatformTime::Seconds() - RefreshStart;
		SpawnSeconds += RefreshTook;
		Captures.Add(Capture);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION capture probe=%s at=(%.0f, %.0f, %.0f) reason=neighbour t=%.2f cells=%d built=%d new_cells=%d pending=%d refresh_ms=%.3f"),
			*Probe->GetName(), Probe->GetActorLocation().X, Probe->GetActorLocation().Y, Probe->GetActorLocation().Z, WorldSeconds, State.Cells, State.Built, NewCells, State.Pending, RefreshTook * 1000.0);
		Entry.SeenCells.Append(State.BuiltCellIds);
		Entry.bWantedBefore = false;
		++Again;
	}

	if (Captures.IsEmpty())
	{
		return;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION spawned=%d recaptured=%d game_ms=%.3f"), First, Again, (FPlatformTime::Seconds() - CheckStart) * 1000.0);
	PumpGameSeconds = SpawnSeconds;
	if (First > 0 && FMantleReflectionProbes::IsLightmapMixingOn())
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_REFLECTION r.ReflectionEnvironmentLightmapMixing is on: runtime captures have no average brightness, so rough surfaces will read far too bright"));
	}
	BeginPumping();
}

void UMantleReflectionProbeSubsystem::BeginPumping()
{
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
	PumpWorstFrameSeconds = 0.0;
	PumpSumFrameSeconds = 0.0;
	PumpStartSeconds = FPlatformTime::Seconds();
	Phase = EPhase::Pumping;
}

void UMantleReflectionProbeSubsystem::PumpOnce(const float DeltaTime)
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
	PumpWorstFrameSeconds = FMath::Max(PumpWorstFrameSeconds, static_cast<double>(DeltaTime));
	PumpSumFrameSeconds += DeltaTime;
	const bool bLongEnough = FPlatformTime::Seconds() - PumpStartSeconds >= FMantleReflectionProbes::MinPumpSeconds;
	if (Valid == 0 || (FramesLeft <= 0 && bLongEnough))
	{
		// What the captures cost: the frames they ran in (the renderer takes a face a frame, off the game thread) and what the game thread paid to start them.
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_REFLECTION captured=%d frames=%d mean_frame_ms=%.2f worst_frame_ms=%.2f start_game_ms=%.3f"),
			Valid, FramesPumped, FramesPumped > 0 ? PumpSumFrameSeconds / FramesPumped * 1000.0 : 0.0, PumpWorstFrameSeconds * 1000.0, PumpGameSeconds * 1000.0);
		Captures.Reset();
		Phase = EPhase::Idle;
		FramesSinceCheck = 0;
		// Probes of a build that ended meanwhile.
		if (Pending.Num() > 0 && Live.IsEmpty())
		{
			BeginWarmup();
		}
	}
}
