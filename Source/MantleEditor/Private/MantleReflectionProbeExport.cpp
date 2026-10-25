// Copyright Joshua Eger. All Rights Reserved.
#include "MantleReflectionProbeExport.h"
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceValidator.h"
#include "MantleTargetFeatures.h"

#include "Components/BoxReflectionCaptureComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Engine/ReflectionCapture.h"
#include "GameFramework/Actor.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleReflectionProbeExportPrivate
{
	FString ProbeExportLabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** Value pulled into [Min, Max]; a change is said in OutApproximations as the value, the range a place holds and the clamped result. */
	double ProbeExportClamp(double Value, double Min, double Max, const TCHAR* What, TArray<FString>& OutApproximations)
	{
		const double Clamped = FMath::Clamp(Value, Min, Max);
		if (Clamped != Value)
		{
			OutApproximations.Add(FString::Printf(TEXT("%s %g is outside what a place holds (%g to %g), so it is %g"), What, Value, Min, Max, Clamped));
		}
		return Clamped;
	}

	/** To four decimals: a float setting such as 0.1 should be written as 0.1, not 0.10000000149. */
	double ProbeExportRounded(double Value)
	{
		return FMath::RoundToDouble(Value * 10000.0) / 10000.0;
	}
}

const UReflectionCaptureComponent* FMantleReflectionProbeExport::MappableCapture(const AActor* Actor)
{
	const AReflectionCapture* Capture = Cast<AReflectionCapture>(Actor);
	const UReflectionCaptureComponent* Component = Capture ? Capture->GetCaptureComponent() : nullptr;
	if (Component && (Component->IsA(UBoxReflectionCaptureComponent::StaticClass()) || Component->IsA(USphereReflectionCaptureComponent::StaticClass())))
	{
		return Component;
	}
	return nullptr;
}

bool FMantleReflectionProbeExport::IsProbeActor(const AActor* Actor)
{
	return MappableCapture(Actor) != nullptr;
}

void FMantleReflectionProbeExport::Read(const AActor* Actor, FMantleReflectionProbeProps& Out, TArray<FString>& OutApproximations)
{
	using namespace MantleReflectionProbeExportPrivate;
	const UReflectionCaptureComponent* Component = MappableCapture(Actor);
	if (!Component)
	{
		return;
	}
	if (const UBoxReflectionCaptureComponent* Box = Cast<UBoxReflectionCaptureComponent>(Component))
	{
		const FVector Scale = Box->GetComponentTransform().GetScale3D();
		Out.Shape = EMantleReflectionProbeShape::Box;
		const double MinExtent = FMantlePlaceLimits::MinReflectionProbeExtent;
		const double MaxExtent = FMantlePlaceLimits::MaxReflectionProbeExtent;
		Out.HalfExtent = FVector(
			ProbeExportRounded(ProbeExportClamp(FMath::Abs(Scale.X), MinExtent, MaxExtent, TEXT("its half extent on X"), OutApproximations)),
			ProbeExportRounded(ProbeExportClamp(FMath::Abs(Scale.Y), MinExtent, MaxExtent, TEXT("its half extent on Y"), OutApproximations)),
			ProbeExportRounded(ProbeExportClamp(FMath::Abs(Scale.Z), MinExtent, MaxExtent, TEXT("its half extent on Z"), OutApproximations)));
		Out.TransitionDistance = ProbeExportRounded(ProbeExportClamp(Box->BoxTransitionDistance, FMantlePlaceLimits::MinReflectionProbeTransition,
			FMantlePlaceLimits::MaxReflectionProbeTransition, TEXT("its transition distance"), OutApproximations));
	}
	else if (const USphereReflectionCaptureComponent* Sphere = Cast<USphereReflectionCaptureComponent>(Component))
	{
		Out.Shape = EMantleReflectionProbeShape::Sphere;
		Out.InfluenceRadius = ProbeExportRounded(ProbeExportClamp(Sphere->InfluenceRadius, FMantlePlaceLimits::MinReflectionProbeRadius,
			FMantlePlaceLimits::MaxReflectionProbeRadius, TEXT("its influence radius"), OutApproximations));
	}
	Out.Brightness = ProbeExportRounded(ProbeExportClamp(Component->Brightness, 0.0, FMantlePlaceLimits::MaxReflectionProbeBrightness,
		TEXT("its brightness"), OutApproximations));
	const double Leak = (Component->RuntimeSkylightScale.R + Component->RuntimeSkylightScale.G + Component->RuntimeSkylightScale.B) / 3.0;
	Out.SkylightScale = ProbeExportRounded(ProbeExportClamp(Leak, 0.0, 1.0, TEXT("its sky light scale"), OutApproximations));
}

FTransform FMantleReflectionProbeExport::PublishedTransform(const AActor* Actor, const AActor* ExportedParent)
{
	const UReflectionCaptureComponent* Component = MappableCapture(Actor);
	if (!Component)
	{
		return FTransform::Identity;
	}
	// The renderer captures from the owner's location, so the offset becomes part of where the probe stands.
	const FTransform World(Component->GetComponentQuat(), Component->GetComponentLocation() + Component->CaptureOffset, FVector::OneVector);
	return ExportedParent ? World.GetRelativeTransform(ExportedParent->GetActorTransform()) : World;
}

void FMantleReflectionProbeExport::Extract(const AActor* Actor, FMantleEntity& Entity)
{
	FMantleReflectionProbeProps Props;
	TArray<FString> Approximations;
	Read(Actor, Props, Approximations);
	Entity.Props.Set<FMantleReflectionProbeProps>(MoveTemp(Props));

	// Only an entity with a parent is relative to it, and Export only parents to an actor it exports too.
	const AActor* ParentActor = Entity.Parent.IsEmpty() ? nullptr : Actor->GetAttachParentActor();
	Entity.Transform = PublishedTransform(Actor, ParentActor);
	Entity.Transform.SetScale3D(FVector::OneVector);
}

bool FMantleReflectionProbeExport::AdmitForExport(const AActor* Actor, int32& InOutAdmitted, FMantleCoverage& Coverage)
{
	using namespace MantleReflectionProbeExportPrivate;
	namespace Cat = MantleCoverageCategory;
	const UReflectionCaptureComponent* Component = MappableCapture(Actor);
	if (!Component)
	{
		return false;
	}
	const FString Label = ProbeExportLabelOf(Actor);
	if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::ReflectionProbes))
	{
		Coverage.Add(Cat::Lights, EMantleCoverageOutcome::Dropped, Label,
			FString::Printf(TEXT("a reflection probe left out: %s"), FMantleTargetFeatures::NeedsNewerClient));
		return false;
	}
	if (!Component->GetVisibleFlag() || Actor->IsHidden())
	{
		Coverage.Add(Cat::Lights, EMantleCoverageOutcome::Dropped, Label, TEXT("a reflection capture that is hidden is not published."));
		return false;
	}
	if (InOutAdmitted >= FMantlePlaceLimits::MaxReflectionProbesPerPlace)
	{
		Coverage.Add(Cat::Lights, EMantleCoverageOutcome::Dropped, Label,
			FString::Printf(TEXT("left out: a place holds at most %d reflection probes."), FMantlePlaceLimits::MaxReflectionProbesPerPlace));
		return false;
	}
	++InOutAdmitted;

	// What the probe is not: a baked or specified capture is replaced by one taken at run time.
	TArray<FString> Notes;
	if (Component->ReflectionSourceType == EReflectionSourceType::SpecifiedCubemap)
	{
		Notes.Add(TEXT("its specified cubemap is not carried: captured at runtime with the place's own lighting"));
	}
	else if (Component->GetMapBuildData() != nullptr)
	{
		Notes.Add(TEXT("its baked capture is replaced: captured at runtime with the place's own lighting"));
	}
	if (!Component->CaptureOffset.IsNearlyZero())
	{
		Notes.Add(FString::Printf(TEXT("its capture offset (%g, %g, %g) is folded into its position, which moves its influence volume too"),
			Component->CaptureOffset.X, Component->CaptureOffset.Y, Component->CaptureOffset.Z));
	}
	if (Component->PostProcessMaterial != nullptr)
	{
		Notes.Add(TEXT("its capture post process material is not carried"));
	}
	FMantleReflectionProbeProps Ignored;
	Read(Actor, Ignored, Notes);
	if (Notes.Num() == 0)
	{
		Coverage.Add(Cat::Lights, EMantleCoverageOutcome::Kept, Label);
	}
	else
	{
		Coverage.Add(Cat::Lights, EMantleCoverageOutcome::Approximated, Label, FString::Join(Notes, TEXT("; ")) + TEXT("."));
	}
	return true;
}

void FMantleReflectionProbeExport::Check(const AActor* Actor, EMantleEntityType Type, int32& InOutProbes, FMantleValidationResult& Result)
{
	if (Type != EMantleEntityType::ReflectionProbe || !IsProbeActor(Actor))
	{
		return;
	}
	const FString Label = MantleReflectionProbeExportPrivate::ProbeExportLabelOf(Actor);
	if (++InOutProbes == FMantlePlaceLimits::MaxReflectionProbesPerPlace + 1)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Warning,
			Label,
			FString::Printf(TEXT("is reflection probe %d; a place holds at most %d, so this one and the rest are left out."),
				InOutProbes, FMantlePlaceLimits::MaxReflectionProbesPerPlace),
			TEXT("Remove some reflection captures: one in each room is enough.") });
	}
	FMantleReflectionProbeProps Ignored;
	TArray<FString> Approximations;
	Read(Actor, Ignored, Approximations);
	if (Approximations.Num() > 0)
	{
		Result.Issues.Add(FMantleValidationIssue{
			EMantleIssueSeverity::Warning,
			Label,
			FString::Printf(TEXT("has a reflection capture setting a place cannot hold: %s."), *FString::Join(Approximations, TEXT("; "))),
			TEXT("It publishes with the nearest value a place holds.") });
	}
}
