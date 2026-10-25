// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairFitReport.h"

bool FMantleHairFitReport::IsBakedHeadName(const FString& SectionName)
{
	const FString Name = SectionName.ToLower();
	if (Name.Contains(TEXT("cap")) || Name.Contains(TEXT("scalp"))) return false;
	return Name.Contains(TEXT("head")) || Name.Contains(TEXT("face")) || Name.Contains(TEXT("skin")) || Name.Contains(TEXT("body"));
}

FMantleHairFitReport::FResult FMantleHairFitReport::Describe(const FInput& Input)
{
	FResult Out;
	Out.Headline = Headline();

	const bool bManyInside = Input.CardVertices > 0
		&& 100.0 * Input.CardInsideAfter / Input.CardVertices > LowInsidePercent;
	const bool bRootsFloat = !Input.bHadCap && Input.RootsSeatedPercent >= 0.0 && Input.RootsSeatedPercent < LowSeatedPercent;
	Out.bLowConfidence = !Input.bKeepsOutOfHead || Input.bScalpCheckFit || bManyInside || bRootsFloat;
	if (Out.bLowConfidence)
	{
		Out.Messages.Add(TEXT("We are not sure this hair sits right on the head. Look closely at the picture; if it is off, press Adjust and the hair uploads where you made it."));
	}

	Out.bBakedHead = Input.bBakedHead;
	if (Input.bBakedHead)
	{
		Out.Messages.Add(FString::Printf(TEXT("This hair file has a head built into it (\"%s\"). We used only the hair to size it, and the built-in head is still in the file, so check that it does not show through or remove it before you upload."),
			*Input.BakedHeadSection));
	}
	return Out;
}

TArray<FVector3f> FMantleHairFitMeshes::HeadInHairFrame() const
{
	TArray<FVector3f> Out = HeadPositions;
	if (!IsValid()) return Out;
	FVector CentreBefore = FVector::ZeroVector, CentreAfter = FVector::ZeroVector;
	for (int32 I = 0; I < AfterPositions.Num(); ++I)
	{
		CentreBefore += FVector(BeforePositions[I]);
		CentreAfter += FVector(AfterPositions[I]);
	}
	CentreBefore /= AfterPositions.Num();
	CentreAfter /= AfterPositions.Num();
	double SpreadBefore = 0.0, SpreadAfter = 0.0;
	for (int32 I = 0; I < AfterPositions.Num(); ++I)
	{
		SpreadBefore += (FVector(BeforePositions[I]) - CentreBefore).SizeSquared();
		SpreadAfter += (FVector(AfterPositions[I]) - CentreAfter).SizeSquared();
	}
	if (SpreadAfter < 1.e-12 || SpreadBefore < 1.e-12) return Out;
	// The fit scaled the hair by Scale about nothing in particular: After = Scale * Before + Shift, so Before = (After - Shift) / Scale.
	const double Scale = FMath::Sqrt(SpreadAfter / SpreadBefore);
	for (FVector3f& P : Out)
	{
		const FVector Moved = (FVector(P) - CentreAfter) / Scale + CentreBefore;
		P = FVector3f(static_cast<float>(Moved.X), static_cast<float>(Moved.Y), static_cast<float>(Moved.Z));
	}
	return Out;
}
