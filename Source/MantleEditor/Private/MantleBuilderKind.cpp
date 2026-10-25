// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBuilderKind.h"

namespace
{
	/** The Y = 0 reflection of a transform with handedness kept: location y negated, rotation conjugated by the reflection. */
	FTransform MirrorAcrossY(const FTransform& T)
	{
		FVector Location = T.GetLocation();
		Location.Y = -Location.Y;
		// An axial vector under a reflection keeps its component along the mirror normal and flips the other two.
		const FQuat Q = T.GetRotation();
		const FQuat Mirrored(-Q.X, Q.Y, -Q.Z, Q.W);
		return FTransform(Mirrored, Location, T.GetScale3D());
	}
}

TArray<FMantleBuilderPoint> MantleBuilderPoints::MergeGuesses(const TArray<FMantleBuilderPoint>& Existing, const TArray<FMantleBuilderPoint>& NewGuesses)
{
	TArray<FMantleBuilderPoint> Out = Existing;
	for (const FMantleBuilderPoint& NewGuess : NewGuesses)
	{
		FMantleBuilderPoint* Found = Out.FindByPredicate([&NewGuess](const FMantleBuilderPoint& P) { return P.Role == NewGuess.Role; });
		if (!Found)
		{
			FMantleBuilderPoint Added = NewGuess;
			Added.Transform = NewGuess.Guess;
			Added.Source = EMantleBuilderPointSource::Guess;
			Out.Add(Added);
			continue;
		}
		Found->Guess = NewGuess.Guess;
		if (Found->Source == EMantleBuilderPointSource::Guess)
		{
			Found->Transform = NewGuess.Guess;
			Found->Parent = NewGuess.Parent;
		}
	}
	return Out;
}

bool MantleBuilderPoints::MirrorPoint(const FMantleBuilderPoint& From, const TArray<FMantleBuilderPointRole>& Roles, FMantleBuilderPoint& OutMirrored)
{
	const FMantleBuilderPointRole* Role = Roles.FindByPredicate([&From](const FMantleBuilderPointRole& R) { return R.Role == From.Role; });
	if (!Role || Role->MirrorRole.IsNone())
	{
		return false;
	}
	FMantleBuilderPoint Result = From;
	Result.Role = Role->MirrorRole;
	Result.Transform = MirrorAcrossY(From.Transform);
	Result.Guess = MirrorAcrossY(From.Guess);
	Result.Source = EMantleBuilderPointSource::Creator;
	OutMirrored = Result;
	return true;
}

void MantleBuilderPoints::ResetAll(TArray<FMantleBuilderPoint>& Points)
{
	for (FMantleBuilderPoint& Point : Points)
	{
		Point.ResetToGuess();
	}
}

const FMantleBuilderPoint* MantleBuilderPoints::Find(const TArray<FMantleBuilderPoint>& Points, FName Role)
{
	return Points.FindByPredicate([Role](const FMantleBuilderPoint& P) { return P.Role == Role; });
}
