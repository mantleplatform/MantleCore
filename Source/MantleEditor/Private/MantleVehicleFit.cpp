// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleFit.h"
#include "MantleVehicleConverter.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleSource.h"

#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "ReferenceSkeleton.h"

// Named, not anonymous: unity builds merge this file with others.
namespace MantleVehicleFitPrivate
{
	/** Wheels whose centres are this close along the vehicle's length (cm) are on the same axle. */
	constexpr double AxleTolerance = 25.0;
	/** A size this close to 1 is left as modelled: a change this small is invisible, and "made 1.03x bigger" is only noise to a creator. */
	constexpr double SnapToOne = 0.05;
	/** The two signals disagree when one asks for more than this times the other. */
	constexpr double DisagreeRatio = 1.2;
	/** A wheel this close to the model's centre line (cm) is on neither side. */
	constexpr double CentreLineTolerance = 5.0;

	/** A size as a creator reads it: hundredths, without trailing zeros. */
	FString SizeText(const double Value)
	{
		return FString::SanitizeFloat(FMath::RoundToDouble(Value * 100.0) / 100.0, 0);
	}

	/** "1.2x bigger" or "smaller (0.8x)". */
	FString ChangeText(const double Scale)
	{
		return Scale > 1.0 ? FString::Printf(TEXT("%sx bigger"), *SizeText(Scale)) : FString::Printf(TEXT("smaller (%sx)"), *SizeText(Scale));
	}

	/** Every bone's reference pose in component space (the raw skeleton lists parents first). */
	TArray<FTransform> FitComponentPose(const FReferenceSkeleton& Skeleton)
	{
		const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
		const TArray<FTransform>& Pose = Skeleton.GetRawRefBonePose();
		TArray<FTransform> Out;
		Out.SetNum(Info.Num());
		for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
		{
			const FTransform Local = Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity;
			const int32 Parent = Info[Bone].ParentIndex;
			Out[Bone] = Parent >= 0 && Parent < Bone ? Local * Out[Parent] : Local;
		}
		return Out;
	}
}

FMantleVehicleFit FMantleVehicleFitter::Compute(const FMantleVehiclePreset& Preset, TConstArrayView<FMantleVehicleFitWheel> Wheels)
{
	using namespace MantleVehicleFitPrivate;
	FMantleVehicleFit Fit;

	// The wheel radius: the mean over the wheels that have one.
	double RadiusSum = 0.0;
	int32 RadiusCount = 0;
	for (const FMantleVehicleFitWheel& Wheel : Wheels)
	{
		if (Wheel.Radius > 0.0)
		{
			RadiusSum += Wheel.Radius;
			++RadiusCount;
		}
	}
	if (RadiusCount > 0)
	{
		Fit.MeanRadius = RadiusSum / RadiusCount;
		if (Preset.FitWheelRadius > 0.0)
		{
			Fit.RadiusScale = Preset.FitWheelRadius / Fit.MeanRadius;
		}
	}

	// The track: axles are wheels at about the same place along the length; an axle with a wheel on each side has a track. Sides are
	// taken from the model's own centre line (the mean of all its wheels), so a model modelled off the origin still has a track.
	TArray<FVector> Sorted;
	double CentreY = 0.0;
	for (const FMantleVehicleFitWheel& Wheel : Wheels)
	{
		Sorted.Add(Wheel.Centre);
		CentreY += Wheel.Centre.Y;
	}
	if (Sorted.Num() > 0)
	{
		CentreY /= Sorted.Num();
	}
	Sorted.Sort([](const FVector& A, const FVector& B) { return A.X < B.X; });
	double TrackSum = 0.0;
	int32 TrackCount = 0;
	for (int32 Start = 0; Start < Sorted.Num();)
	{
		double MinY = Sorted[Start].Y;
		double MaxY = Sorted[Start].Y;
		int32 End = Start + 1;
		while (End < Sorted.Num() && Sorted[End].X - Sorted[Start].X <= AxleTolerance)
		{
			MinY = FMath::Min(MinY, Sorted[End].Y);
			MaxY = FMath::Max(MaxY, Sorted[End].Y);
			++End;
		}
		if (End - Start >= 2 && MinY < CentreY - CentreLineTolerance && MaxY > CentreY + CentreLineTolerance)
		{
			TrackSum += MaxY - MinY;
			++TrackCount;
		}
		Start = End;
	}
	if (TrackCount > 0)
	{
		Fit.MeanTrack = TrackSum / TrackCount;
		if (Preset.FitTrackWidth > 0.0)
		{
			Fit.TrackScale = Preset.FitTrackWidth / Fit.MeanTrack;
		}
	}

	const bool bHasRadius = Fit.RadiusScale > 0.0;
	const bool bHasTrack = Fit.TrackScale > 0.0;
	if (!bHasRadius && !bHasTrack)
	{
		Fit.Reason = TEXT("Mantle could not tell how big this vehicle is, because it could not measure its wheels. It was left as modelled.");
		return Fit;
	}

	double Raw = 1.0;
	if (bHasRadius && bHasTrack)
	{
		Raw = FMath::Sqrt(Fit.RadiusScale * Fit.TrackScale);
		const double Larger = FMath::Max(Fit.RadiusScale, Fit.TrackScale);
		const double Smaller = FMath::Min(Fit.RadiusScale, Fit.TrackScale);
		Fit.bSignalsDisagree = Larger / Smaller > DisagreeRatio;
	}
	else
	{
		Raw = bHasRadius ? Fit.RadiusScale : Fit.TrackScale;
	}

	const double Unclamped = Raw;
	if (Preset.FitMinScale > 0.0 && Raw < Preset.FitMinScale)
	{
		Raw = Preset.FitMinScale;
		Fit.bClamped = true;
	}
	if (Preset.FitMaxScale > 0.0 && Raw > Preset.FitMaxScale)
	{
		Raw = Preset.FitMaxScale;
		Fit.bClamped = true;
	}
	Fit.Scale = FMath::RoundToDouble(Raw * 100.0) / 100.0;
	if (FMath::Abs(Fit.Scale - 1.0) < SnapToOne)
	{
		Fit.Scale = 1.0;
	}

	const FString Kind = Preset.Name.IsEmpty() ? Preset.Id : Preset.Name;
	// A clamped result may land on 1 (or beside it), so its wording follows the direction the model asked for, not the final size.
	const bool bBigger = Fit.bClamped ? Unclamped > 1.0 : Fit.Scale > 1.0;
	FString Reason;
	if (Fit.bSignalsDisagree)
	{
		const FString Outcome = Fit.Scale == 1.0 ? FString(TEXT("left as modelled")) : TEXT("made ") + ChangeText(Fit.Scale);
		Reason = FString::Printf(TEXT("Its wheels suggest %sx but its track suggests %sx, so it was %s. Check the driver in the preview and adjust the size if it looks wrong."),
			*SizeText(Fit.RadiusScale), *SizeText(Fit.TrackScale), *Outcome);
	}
	else if (Fit.bClamped && Fit.Scale == 1.0)
	{
		const FString Parts = bHasRadius && bHasTrack ? TEXT("its wheels and track are") : bHasRadius ? TEXT("its wheels are") : TEXT("its track is");
		Reason = FString::Printf(TEXT("%s %s than a typical %s, but it was left as modelled."),
			*(Parts.Left(1).ToUpper() + Parts.Mid(1)), bBigger ? TEXT("smaller") : TEXT("bigger"), *Kind);
	}
	else if (Fit.Scale == 1.0)
	{
		Reason = FString::Printf(TEXT("Already the right size for a %s."), *Kind);
	}
	else
	{
		const FString Parts = bHasRadius && bHasTrack ? TEXT("its wheels and track are") : bHasRadius ? TEXT("its wheels are") : TEXT("its track is");
		Reason = FString::Printf(TEXT("Made %s so a Mantle driver fits: %s %s than a typical %s."),
			*ChangeText(Fit.Scale), *Parts, bBigger ? TEXT("smaller") : TEXT("bigger"), *Kind);
	}
	if (Fit.bClamped)
	{
		Reason += FString::Printf(TEXT(" That is the most a %s is %s automatically; change the size by hand if it still looks wrong."), *Kind, bBigger ? TEXT("enlarged") : TEXT("shrunk"));
	}
	Fit.Reason = Reason;
	return Fit;
}

FMantleVehicleFit FMantleVehicleFitter::ComputeFromFootprint(const FMantleVehiclePreset& Preset, const double ModelLength, const double ModelWidth, const double ChassisLength,
	const double ChassisWidth)
{
	using namespace MantleVehicleFitPrivate;
	FMantleVehicleFit Fit;
	const FString Kind = Preset.Name.IsEmpty() ? Preset.Id : Preset.Name;
	if (ModelLength < 1.0 || ModelWidth < 1.0 || ChassisLength < 1.0 || ChassisWidth < 1.0)
	{
		Fit.Reason = FString::Printf(TEXT("Mantle could not tell how big your model is, so the %s was left at its standard size."), *Kind);
		return Fit;
	}
	double Raw = FMath::Sqrt((ModelLength * ModelWidth) / (ChassisLength * ChassisWidth));
	const double Unclamped = Raw;
	if (Preset.FitMinScale > 0.0 && Raw < Preset.FitMinScale)
	{
		Raw = Preset.FitMinScale;
		Fit.bClamped = true;
	}
	if (Preset.FitMaxScale > 0.0 && Raw > Preset.FitMaxScale)
	{
		Raw = Preset.FitMaxScale;
		Fit.bClamped = true;
	}
	Fit.Scale = FMath::RoundToDouble(Raw * 100.0) / 100.0;
	Fit.RadiusScale = 0.0;
	Fit.TrackScale = 0.0;
	if (Fit.bClamped)
	{
		const bool bTooBig = Unclamped > Fit.Scale;
		Fit.Reason = FString::Printf(TEXT("Your model is %s than a %s goes (it covers %sx the ground of a standard one), so the %s is %sx its standard size and your model is %s to fit it."),
			bTooBig ? TEXT("bigger") : TEXT("smaller"), *Kind, *SizeText(Unclamped), *Kind, *SizeText(Fit.Scale), bTooBig ? TEXT("shrunk") : TEXT("enlarged"));
	}
	else
	{
		Fit.Reason = FString::Printf(TEXT("Sized to your model: the %s is %sx its standard size, so it covers as much ground as your model does."), *Kind, *SizeText(Fit.Scale));
	}
	return Fit;
}

bool FMantleVehicleFitter::WheelsFromMesh(const USkeletalMesh& Mesh, const UMantleVehicleSource& Source, TArray<FMantleVehicleFitWheel>& Out)
{
	using namespace MantleVehicleFitPrivate;
	Out.Reset();
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	const TArray<FTransform> Pose = FitComponentPose(Skeleton);

	// The body as it uploads, for the wheels with no radius of their own; without it those wheels count towards the track alone.
	FMantleGlbMesh Body;
	bool bBodyRead = false;
	bool bTriedBody = false;

	for (const FMantleVehicleSourceWheel& Entry : Source.Wheels)
	{
		const int32 Bone = Skeleton.FindRawBoneIndex(Entry.Bone);
		if (!Pose.IsValidIndex(Bone))
		{
			continue;
		}
		FMantleVehicleFitWheel Wheel;
		Wheel.Centre = Pose[Bone].GetLocation();
		if (Entry.Radius > 0.f)
		{
			Wheel.Radius = Entry.Radius;
		}
		else
		{
			if (!bTriedBody)
			{
				bTriedBody = true;
				TArray<UMaterialInterface*> Materials;
				FString Error;
				bBodyRead = FMantleVehicleConverter::ReadBody(Source, Body, Materials, Error);
			}
			double Radius = 0.0;
			double Width = 0.0;
			if (bBodyRead && FMantleVehicleConverter::MeasureWheel(Body, Entry.Bone, Radius, Width))
			{
				Wheel.Radius = Radius;
			}
		}
		Out.Add(Wheel);
	}
	return Out.Num() > 0;
}
