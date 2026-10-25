// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairRoute.h"

FMantleHairRoute::FResult FMantleHairRoute::Decide(const FInput& Input)
{
	FResult Out;
	if (Input.bGroom)
	{
		Out.Kind = EKind::Groom;
		Out.Title = TEXT("MetaHuman groom");
		Out.Words = TEXT("We use the groom's own hair cards and their textures. Nothing is resized.");
		return Out;
	}
	if (Input.bSkeletalMesh)
	{
		Out.Kind = EKind::SkeletalHair;
		Out.Title = TEXT("Skeletal hair");
		Out.Words = TEXT("We use the finest version of this hair that fits the triangle limit, attached to the head.");
		return Out;
	}

	// A static cards mesh: solid sculpted hair has almost no separate pieces and one huge one.
	const bool bSolid = !Input.bMetaHumanOrigin && Input.Triangles >= SculptedMinTriangles && Input.Pieces >= 1 && Input.Pieces <= SculptedMaxPieces
		&& Input.LargestPieceTriangles >= SculptedLargestShare * Input.Triangles;
	if (bSolid)
	{
		Out.Kind = EKind::SculptedSolid;
		Out.bSupported = false;
		Out.Title = TEXT("Solid sculpted hair");
		Out.Words = TEXT("This looks like one solid hair mesh, not hair cards. That kind is not supported yet and needs a sample to build for, so it may come out thin or broken.");
		return Out;
	}
	if (Input.bMetaHumanOrigin)
	{
		Out.Kind = EKind::MetaHumanCards;
		Out.Title = TEXT("MetaHuman hair cards");
		Out.Words = TEXT("These cards were made on the MetaHuman head, so we keep them where they are.");
		return Out;
	}

	Out.bFitsToHead = !Input.bPlacedByHand;
	const bool bOver = Input.Budget > 0 && Input.Triangles > Input.Budget;
	Out.Kind = bOver ? EKind::DenseCards : EKind::BoughtCards;
	Out.Title = bOver ? TEXT("Dense hair cards") : TEXT("Bought hair cards");
	FString Sizing = Input.bPlacedByHand ? TEXT("The hair uploads where you placed it.") : TEXT("We size the hair to the standard head and sit it on the scalp.");
	if (bOver)
	{
		Out.bThinned = true;
		Out.KeptPercent = FMath::Clamp(FMath::RoundToInt(100.0 * Input.Budget / Input.Triangles), 1, 99);
		Out.Words = FString::Printf(TEXT("%s This hair has %d triangles and the limit is %d, so we keep about %d%% of the cards, spread evenly: thin, but even."),
			*Sizing, Input.Triangles, Input.Budget, Out.KeptPercent);
	}
	else
	{
		Out.Words = Sizing;
	}
	return Out;
}
