// Copyright Joshua Eger. All Rights Reserved.
#include "MantleModelThumbnail.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleVehicleThumbnail.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"

FRotator FMantleModelThumbnail::DefaultView()
{
	return FMantleVehicleThumbnail::View();
}

FMantleThumbnailSubject FMantleModelThumbnail::Subject(UObject* Model)
{
	FMantleThumbnailSubject Out;
	if (UStaticMesh* Static = Cast<UStaticMesh>(Model))
	{
		Out.StaticMesh = Static;
		Out.Frame = Static->GetBounds().GetBox();
		Out.Describe = Static->GetName();
	}
	else if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Model))
	{
		Out.SkeletalMeshes.Add(Skeletal);
		Out.Frame = Skeletal->GetBounds().GetBox();
		Out.Describe = Skeletal->GetName();
	}
	Out.View = DefaultView();
	return Out;
}

void FMantleModelThumbnail::ApplyChoice(FMantleThumbnailSubject& Subject, const FMantleThumbnailChoice& Choice)
{
	if (Choice.Mode != EMantleThumbnailMode::Angle)
	{
		return;
	}
	Subject.View = FRotator(Choice.Angle.Pitch, Choice.Angle.Yaw, 0.0);
	if (Subject.Frame.IsValid && !FMath::IsNearlyEqual(Choice.Zoom, 1.0f))
	{
		Subject.Frame = FBox::BuildAABB(Subject.Frame.GetCenter(), Subject.Frame.GetExtent() / FMath::Max(Choice.Zoom, 0.1f));
	}
	Subject.Describe += TEXT(", chosen angle");
}

bool FMantleModelThumbnail::Render(UObject* Model, const FMantleThumbnailChoice& Choice, int32 Size, TArray<FColor>& OutPixels, FString& OutError)
{
	FMantleThumbnailSubject Shot = Subject(Model);
	if (Shot.IsEmpty())
	{
		OutError = TEXT("there is no 3D model to draw");
		return false;
	}
	ApplyChoice(Shot, Choice);
	if (!FMantleCosmeticThumbnail::CanCapture(&OutError))
	{
		return false;
	}
	return FMantleCosmeticThumbnail::Capture(Shot, Size, OutPixels, OutError);
}

FMantleThumbnailResolver::FRender FMantleModelThumbnail::RenderFor(TFunction<UObject*()> GetModel)
{
	return [GetModel](const FMantleThumbnailChoice& Choice, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		OutWidth = OutHeight = FMantleThumbnailTarget::Square().Width;
		return Render(GetModel ? GetModel() : nullptr, Choice, OutWidth, OutPixels, OutError);
	};
}
