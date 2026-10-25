// Copyright Joshua Eger. All Rights Reserved.
#include "MantleUnriggedAsIs.h"
#include "MantleAvatarHeight.h"
#include "MantleMetaHumanConform.h"
#include "MantleMetaHumanPose.h"
#include "MantleRuntime.h"
#include "MantleUnriggedBody.h"

#include "AnimationRuntime.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshQueries.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "RHIGlobals.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

using UE::AnimationCore::FBoneWeight;

const FName FMantleUnriggedAsIs::HeadBone(TEXT("head"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUnriggedAsIsPrivate
{
	const TCHAR* const AsIsAvatarSlot = TEXT("avatar");

	/** Influences summed by bone, then the heaviest Max kept and renormalised; false when nothing weighs anything. */
	bool KeepHeaviest(TArray<TPair<int32, double>>& Blend, const int32 Max)
	{
		Blend.RemoveAll([](const TPair<int32, double>& Entry) { return !(Entry.Value > 0.0); });
		Blend.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });
		if (Blend.Num() > Max)
		{
			Blend.SetNum(Max);
		}
		double Sum = 0.0;
		for (const TPair<int32, double>& Entry : Blend)
		{
			Sum += Entry.Value;
		}
		if (!(Sum > 0.0))
		{
			return false;
		}
		for (TPair<int32, double>& Entry : Blend)
		{
			Entry.Value /= Sum;
		}
		return true;
	}

	void AddTo(TArray<TPair<int32, double>>& Blend, const int32 Bone, const double Weight)
	{
		if (TPair<int32, double>* Existing = Blend.FindByPredicate([Bone](const TPair<int32, double>& Entry) { return Entry.Key == Bone; }))
		{
			Existing->Value += Weight;
		}
		else
		{
			Blend.Emplace(Bone, Weight);
		}
	}

	/** A skinned or static mesh source with no rig of its own (one bone, or no usable weights). */
	bool SkeletalHasNoRig(const USkeletalMesh& Mesh)
	{
		return Mesh.GetRefSkeleton().GetRawBoneNum() == 1 || FMantleUnriggedBody::HasNoUsableWeights(FMantleUnriggedBody::ReadSkinWeights(Mesh));
	}

	/** Slot names made unique ("Skin", "Skin_1"): the build matches sections to slots by name. */
	TArray<FName> UniqueSlotNames(const TArray<FStaticMaterial>& Materials)
	{
		TArray<FName> Out;
		TSet<FName> Taken;
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			FName Name = Materials[Slot].MaterialSlotName;
			if (Name.IsNone())
			{
				Name = FName(*FString::Printf(TEXT("Slot_%d"), Slot));
			}
			FName Unique = Name;
			for (int32 Suffix = 1; Taken.Contains(Unique); ++Suffix)
			{
				Unique = FName(*FString::Printf(TEXT("%s_%d"), *Name.ToString(), Suffix));
			}
			Taken.Add(Unique);
			Out.Add(Unique);
		}
		return Out;
	}
}

int32 FMantleAutoRigSkin::AddVertex(const FVector3f& Position, const TConstArrayView<TPair<int32, float>> Influences)
{
	TArray<TPair<int32, double>> Blend;
	for (const TPair<int32, float>& Influence : Influences)
	{
		MantleUnriggedAsIsPrivate::AddTo(Blend, Influence.Key, Influence.Value);
	}
	Blend.RemoveAll([](const TPair<int32, double>& Entry) { return !(Entry.Value > 0.0); });
	Blend.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });
	const int32 Index = Positions.Add(Position);
	for (int32 Slot = 0; Slot < NumInfluences; ++Slot)
	{
		Bones.Add(Blend.IsValidIndex(Slot) ? Blend[Slot].Key : 0);
		Weights.Add(Blend.IsValidIndex(Slot) ? static_cast<float>(Blend[Slot].Value) : 0.0f);
	}
	return Index;
}

float FMantleAutoRigWeights::WeightOf(const int32 Point, const int32 Bone) const
{
	float Out = 0.0f;
	for (int32 Slot = 0; Slot < NumInfluences; ++Slot)
	{
		const int32 Index = Point * NumInfluences + Slot;
		if (Bones.IsValidIndex(Index) && Bones[Index] == Bone && Weights.IsValidIndex(Index))
		{
			Out += Weights[Index];
		}
	}
	return Out;
}

int32 FMantleAutoRigWeights::Dominant(const int32 Point) const
{
	int32 Best = INDEX_NONE;
	float BestWeight = 0.0f;
	for (int32 Slot = 0; Slot < NumInfluences; ++Slot)
	{
		const int32 Index = Point * NumInfluences + Slot;
		if (Weights.IsValidIndex(Index) && Weights[Index] > BestWeight)
		{
			Best = Bones[Index];
			BestWeight = Weights[Index];
		}
	}
	return Best;
}

FString FMantleAutoRigWeights::ToString() const
{
	return FString::Printf(TEXT("%d points: %d above the neck on head, %d from the skin (mean %.2f cm, largest %.2f cm, %d over %.0f cm)"),
		HeadPoints + SkinPoints, HeadPoints, SkinPoints, MeanDistanceCm, MaxDistanceCm, FarPoints, FMantleUnriggedAsIs::FarCm);
}

FString FMantleFingerFix::ToString() const
{
	FString Dropped;
	for (const TPair<FName, int32>& Finger : DroppedFrom)
	{
		Dropped += FString::Printf(TEXT("%s%s %d"), Dropped.IsEmpty() ? TEXT("; dropped from ") : TEXT(", "), *Finger.Key.ToString(), Finger.Value);
	}
	FString Distances;
	for (const TPair<FName, FVector2D>& Finger : DistanceTo)
	{
		Distances += FString::Printf(TEXT("%s%s %.2f cm over %d (chain %.1f cm)"), Distances.IsEmpty() ? TEXT("; skin to its finger's bones: ") : TEXT(", "),
			*Finger.Key.ToString(), Finger.Value.Y > 0.0 ? Finger.Value.X / Finger.Value.Y : 0.0, static_cast<int32>(Finger.Value.Y), ChainLength.FindRef(Finger.Key));
	}
	FString Rigid;
	for (const FName& Hand : RigidHands)
	{
		Rigid += FString::Printf(TEXT("%s%s"), Rigid.IsEmpty() ? TEXT("; fingers following the hand rigidly (too far from their bones): ") : TEXT(", "), *Hand.ToString());
	}
	return FString::Printf(TEXT("%d finger points: %d on two or more fingers, %d kept to their nearest finger%s%s%s"), FingerPoints, MixedPoints, Changed, *Dropped,
		*Distances, *Rigid);
}

FMantleFingerFix FMantleUnriggedAsIs::KeepOneFinger(const TConstArrayView<FVector3f> Points, const TConstArrayView<FName> Names,
	const TConstArrayView<int32> Parents, const TConstArrayView<FVector3f> Joints, FMantleAutoRigWeights& InOut)
{
	FMantleFingerFix Out;
	const int32 NumBones = Names.Num();
	if (Parents.Num() != NumBones || Joints.Num() != NumBones || InOut.NumInfluences <= 0)
	{
		return Out;
	}
	// Each bone's finger: the root of its chain (a child of a hand bone, named for a finger), and that hand.
	const TCHAR* const FingerWords[] = { TEXT("thumb"), TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky") };
	auto IsFingerRoot = [&](const int32 Bone)
	{
		const int32 Parent = Parents[Bone];
		if (Parent < 0 || Parent >= NumBones || !Names[Parent].ToString().StartsWith(TEXT("hand_")))
		{
			return false;
		}
		const FString Name = Names[Bone].ToString();
		for (const TCHAR* Word : FingerWords)
		{
			if (Name.StartsWith(Word))
			{
				return true;
			}
		}
		return false;
	};
	TArray<int32> FingerOf;
	FingerOf.Init(INDEX_NONE, NumBones);
	TArray<int32> HandOf;
	HandOf.Init(INDEX_NONE, NumBones);
	// The main bones: <finger>_metacarpal, _01, _02, _03 and the side; not the helper bones Epic's rig logic drives.
	auto IsMainBone = [&](const int32 Bone)
	{
		const FString Name = Names[Bone].ToString();
		for (const TCHAR* Word : FingerWords)
		{
			if (Name.StartsWith(Word))
			{
				const FString Rest = Name.RightChop(FCString::Strlen(Word));
				for (const TCHAR* Part : { TEXT("_metacarpal_"), TEXT("_01_"), TEXT("_02_"), TEXT("_03_") })
				{
					if (Rest.StartsWith(Part) && Rest.Len() == FCString::Strlen(Part) + 1)
					{
						return true;
					}
				}
			}
		}
		return false;
	};
	TArray<bool> HasChild;
	HasChild.Init(false, NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		if (Parents[Bone] >= 0 && Parents[Bone] < NumBones && IsMainBone(Bone))
		{
			HasChild[Parents[Bone]] = true;
		}
		for (int32 Walk = Bone; Walk >= 0 && Walk < NumBones; Walk = Parents[Walk])
		{
			if (IsFingerRoot(Walk))
			{
				FingerOf[Bone] = Walk;
				HandOf[Bone] = Parents[Walk];
				break;
			}
		}
	}
	// The fingers' bones as segments: parent joint to joint, a fingertip carried on half its bone's length past its last joint.
	struct FFingerSegment
	{
		FVector3f A;
		FVector3f B;
		int32 Bone;
	};
	TArray<FFingerSegment> Segments;
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		const int32 Parent = Parents[Bone];
		if (FingerOf[Bone] == INDEX_NONE || Parent < 0 || !IsMainBone(Bone))
		{
			continue;
		}
		Segments.Add({ Joints[Parent], Joints[Bone], Bone });
		if (!HasChild[Bone])
		{
			Segments.Add({ Joints[Bone], Joints[Bone] + 0.5f * (Joints[Bone] - Joints[Parent]), Bone });
		}
	}
	if (Segments.IsEmpty())
	{
		return Out;
	}
	for (const FFingerSegment& Segment : Segments)
	{
		// The chain: hand to the last joint (the fingertip carried on past it is not bone).
		if (Segment.A != Joints[Segment.Bone])
		{
			Out.ChainLength.FindOrAdd(Names[FingerOf[Segment.Bone]]) += FVector3f::Dist(Segment.A, Segment.B);
		}
	}
	const int32 Slots = InOut.NumInfluences;
	TMap<int32, FVector2D> ByHand;
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		// The fingers (and the hand) its weights reach.
		TSet<int32> Fingers;
		int32 Hand = INDEX_NONE;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const int32 Index = Point * Slots + Slot;
			const int32 Bone = InOut.Bones.IsValidIndex(Index) ? InOut.Bones[Index] : INDEX_NONE;
			if (InOut.Weights.IsValidIndex(Index) && InOut.Weights[Index] > 0.0f && FingerOf.IsValidIndex(Bone) && FingerOf[Bone] != INDEX_NONE)
			{
				Fingers.Add(FingerOf[Bone]);
				Hand = HandOf[Bone];
			}
		}
		if (Fingers.IsEmpty())
		{
			continue;
		}
		++Out.FingerPoints;
		Out.MixedPoints += Fingers.Num() > 1 ? 1 : 0;
		// The nearest finger bone of that hand.
		int32 Nearest = INDEX_NONE;
		float NearestSquared = TNumericLimits<float>::Max();
		for (const FFingerSegment& Segment : Segments)
		{
			if (HandOf[Segment.Bone] != Hand)
			{
				continue;
			}
			const FVector3f Closest = FMath::ClosestPointOnSegment(Points[Point], Segment.A, Segment.B);
			const float Squared = FVector3f::DistSquared(Points[Point], Closest);
			if (Squared < NearestSquared)
			{
				NearestSquared = Squared;
				Nearest = Segment.Bone;
			}
		}
		if (Nearest == INDEX_NONE)
		{
			continue;
		}
		FVector2D& Distance = Out.DistanceTo.FindOrAdd(Names[FingerOf[Nearest]]);
		Distance.X += FMath::Sqrt(NearestSquared);
		Distance.Y += 1.0;
		FVector2D& HandDistance = ByHand.FindOrAdd(Hand);
		HandDistance.X += FMath::Sqrt(NearestSquared);
		HandDistance.Y += 1.0;
		// Its weights on the other fingers dropped, the rest renormalised.
		const int32 Keep = FingerOf[Nearest];
		bool bDropped = false;
		TSet<int32> DroppedHere;
		float Sum = 0.0f;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const int32 Index = Point * Slots + Slot;
			const int32 Bone = InOut.Bones[Index];
			if (InOut.Weights[Index] > 0.0f && FingerOf.IsValidIndex(Bone) && FingerOf[Bone] != INDEX_NONE && FingerOf[Bone] != Keep)
			{
				InOut.Weights[Index] = 0.0f;
				if (!bDropped || !DroppedHere.Contains(FingerOf[Bone]))
				{
					DroppedHere.Add(FingerOf[Bone]);
					++Out.DroppedFrom.FindOrAdd(Names[FingerOf[Bone]]);
				}
				bDropped = true;
			}
			Sum += InOut.Weights[Index];
		}
		if (!bDropped)
		{
			continue;
		}
		++Out.Changed;
		if (Sum > 0.0f)
		{
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				InOut.Weights[Point * Slots + Slot] /= Sum;
			}
		}
		else
		{
			InOut.Bones[Point * Slots] = Nearest;
			InOut.Weights[Point * Slots] = 1.0f;
		}
	}

	// A hand whose finger skin lies far from its finger bones: its fingers follow the hand rigidly.
	TSet<int32> Rigid;
	for (const TPair<int32, FVector2D>& Hand : ByHand)
	{
		if (Hand.Value.Y > 0.0 && Hand.Value.X / Hand.Value.Y > FingerReachCm)
		{
			Rigid.Add(Hand.Key);
			Out.RigidHands.Add(Names[Hand.Key]);
		}
	}
	if (Rigid.IsEmpty())
	{
		return Out;
	}
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		int32 Hand = INDEX_NONE;
		float Moved = 0.0f;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const int32 Index = Point * Slots + Slot;
			const int32 Bone = InOut.Bones[Index];
			if (InOut.Weights[Index] > 0.0f && FingerOf.IsValidIndex(Bone) && FingerOf[Bone] != INDEX_NONE && Rigid.Contains(HandOf[Bone]))
			{
				Hand = HandOf[Bone];
				Moved += InOut.Weights[Index];
				InOut.Weights[Index] = 0.0f;
			}
		}
		if (Hand == INDEX_NONE)
		{
			continue;
		}
		// Onto the hand's slot, else the first empty one (the moved weight left at least one).
		int32 Target = INDEX_NONE;
		for (int32 Slot = 0; Slot < Slots && Target == INDEX_NONE; ++Slot)
		{
			Target = InOut.Bones[Point * Slots + Slot] == Hand && InOut.Weights[Point * Slots + Slot] > 0.0f ? Slot : INDEX_NONE;
		}
		for (int32 Slot = 0; Slot < Slots && Target == INDEX_NONE; ++Slot)
		{
			Target = InOut.Weights[Point * Slots + Slot] <= 0.0f ? Slot : INDEX_NONE;
		}
		InOut.Bones[Point * Slots + Target] = Hand;
		InOut.Weights[Point * Slots + Target] += Moved;
	}
	return Out;
}

void FMantleUnriggedAsIs::MoveByWeights(const TArrayView<FVector3f> Points, const int32 NumInfluences, const TConstArrayView<int32> Bones,
	const TConstArrayView<float> Weights, const TConstArrayView<FTransform> From, const TConstArrayView<FTransform> To, const bool bInverse)
{
	if (NumInfluences <= 0 || From.Num() != To.Num())
	{
		return;
	}
	// Each bone's move as a matrix: out of its From transform, into its To one.
	TArray<FMatrix> Moves;
	Moves.Reserve(From.Num());
	for (int32 Bone = 0; Bone < From.Num(); ++Bone)
	{
		Moves.Add((From[Bone].Inverse() * To[Bone]).ToMatrixWithScale());
	}
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		FMatrix Blend(ForceInitToZero);
		float Total = 0.0f;
		for (int32 Slot = 0; Slot < NumInfluences; ++Slot)
		{
			const int32 Index = Point * NumInfluences + Slot;
			const int32 Bone = Bones.IsValidIndex(Index) ? Bones[Index] : INDEX_NONE;
			const float Weight = Weights.IsValidIndex(Index) ? Weights[Index] : 0.0f;
			if (Weight > 0.0f && Moves.IsValidIndex(Bone))
			{
				Blend += Moves[Bone] * Weight;
				Total += Weight;
			}
		}
		if (!(Total > 0.0f))
		{
			continue;
		}
		Blend *= 1.0f / Total;
		if (bInverse)
		{
			if (FMath::IsNearlyZero(Blend.Determinant()))
			{
				continue;
			}
			Blend = Blend.Inverse();
		}
		Points[Point] = FVector3f(FVector(Blend.TransformPosition(FVector(Points[Point]))));
	}
}

FTransform FMantleUnriggedAsIs::FitRigid(const TConstArrayView<FVector3f> Source, const TConstArrayView<FVector3f> Target, const FTransform& Start,
	const int32 Rounds, double& OutStartCm, double& OutFitCm)
{
	OutStartCm = 0.0;
	OutFitCm = 0.0;
	if (Source.IsEmpty() || Target.IsEmpty())
	{
		return Start;
	}
	// Each source point moved by Current and its nearest target; the closest FitKeepShare of those pairs, and their mean distance.
	struct FPair
	{
		FVector A;
		FVector B;
		double Distance;
	};
	TArray<FPair> Pairs;
	auto Match = [&](const FTransform& Current)
	{
		Pairs.Reset();
		for (const FVector3f& Point : Source)
		{
			const FVector A = Current.TransformPosition(FVector(Point));
			double Best = TNumericLimits<double>::Max();
			FVector Nearest = A;
			for (const FVector3f& Candidate : Target)
			{
				const double Squared = FVector::DistSquared(A, FVector(Candidate));
				if (Squared < Best)
				{
					Best = Squared;
					Nearest = FVector(Candidate);
				}
			}
			Pairs.Add({ A, Nearest, FMath::Sqrt(Best) });
		}
		Pairs.Sort([](const FPair& X, const FPair& Y) { return X.Distance < Y.Distance; });
		Pairs.SetNum(FMath::Max(3, FMath::CeilToInt32(Pairs.Num() * FitKeepShare)), EAllowShrinking::No);
		double Sum = 0.0;
		for (const FPair& Pair : Pairs)
		{
			Sum += Pair.Distance;
		}
		return Pairs.IsEmpty() ? 0.0 : Sum / Pairs.Num();
	};
	// Every round runs (a round can bring the kept pairs apart before a better match), the closest kept.
	FTransform Current = Start;
	FTransform Best = Start;
	OutStartCm = Match(Current);
	OutFitCm = OutStartCm;
	for (int32 Round = 0; Round < Rounds && Pairs.Num() >= 3; ++Round)
	{
		// Horn's closed form: the rotation is the top eigenvector of a 4x4 symmetric matrix of the centred pairs' cross sums.
		FVector CentreA = FVector::ZeroVector;
		FVector CentreB = FVector::ZeroVector;
		for (const FPair& Pair : Pairs)
		{
			CentreA += Pair.A;
			CentreB += Pair.B;
		}
		CentreA /= Pairs.Num();
		CentreB /= Pairs.Num();
		double S[3][3] = {};
		for (const FPair& Pair : Pairs)
		{
			const FVector A = Pair.A - CentreA;
			const FVector B = Pair.B - CentreB;
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Column = 0; Column < 3; ++Column)
				{
					S[Row][Column] += A[Row] * B[Column];
				}
			}
		}
		double N[4][4] = {
			{ S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
			{ S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
			{ S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
			{ S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] } };
		// Shifted to make every eigenvalue positive, then squared over and over: its columns turn toward the top eigenvector.
		double Shift = 0.0;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				Shift += FMath::Abs(N[Row][Column]);
			}
		}
		for (int32 Row = 0; Row < 4; ++Row)
		{
			N[Row][Row] += Shift;
		}
		for (int32 Square = 0; Square < 24; ++Square)
		{
			double Product[4][4] = {};
			double Largest = 0.0;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					for (int32 K = 0; K < 4; ++K)
					{
						Product[Row][Column] += N[Row][K] * N[K][Column];
					}
					Largest = FMath::Max(Largest, FMath::Abs(Product[Row][Column]));
				}
			}
			if (!(Largest > 0.0))
			{
				break;
			}
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					N[Row][Column] = Product[Row][Column] / Largest;
				}
			}
		}
		int32 Top = 0;
		double BestNorm = -1.0;
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const double Norm = FMath::Square(N[0][Column]) + FMath::Square(N[1][Column]) + FMath::Square(N[2][Column]) + FMath::Square(N[3][Column]);
			if (Norm > BestNorm)
			{
				BestNorm = Norm;
				Top = Column;
			}
		}
		FQuat Turn(N[1][Top], N[2][Top], N[3][Top], N[0][Top]);
		if (!(BestNorm > 0.0))
		{
			break;
		}
		Turn.Normalize();
		// About the pairs' centre: off the source's centre, turned, onto the target's.
		Current = Current * (FTransform(-CentreA) * FTransform(Turn) * FTransform(CentreB));
		const double Mean = Match(Current);
		if (Mean < OutFitCm)
		{
			OutFitCm = Mean;
			Best = Current;
		}
	}
	return Best;
}

bool FMantleUnriggedAsIs::HasNoRig(const UObject* Selection)
{
	if (Cast<UStaticMesh>(Selection))
	{
		return true;
	}
	const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Selection);
	return Mesh && MantleUnriggedAsIsPrivate::SkeletalHasNoRig(*Mesh);
}

bool FMantleUnriggedAsIs::CanAutoRig(const UObject* Selection, FString& OutNote)
{
	OutNote.Reset();
	if (!HasNoRig(Selection))
	{
		return false;
	}
	// The conform takes a front portrait of the face (MHConform's own refusals, in the same words).
	if (GUsingNullRHI)
	{
		OutNote = TEXT("It has no rig, and fitting one needs a renderer, which this editor runs without.");
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		OutNote = TEXT("It has no rig, and fitting one needs the editor's UI, which this editor runs without.");
		return false;
	}
	OutNote = PlainMessage();
	return true;
}

FString FMantleUnriggedAsIs::PlainMessage()
{
	return TEXT("This model has no rig, so we'll fit Epic's MetaHuman skeleton inside it and skin it to that. It keeps its own look.");
}

bool FMantleUnriggedAsIs::IsUnriggedAsIs(const FMantleConvertContext& Context, const int32 PartIndex)
{
	if (Context.Route != EMantleAvatarRoute::AsIs || !Context.AssemblyFolder.IsEmpty() || !Context.Parts.IsValidIndex(PartIndex))
	{
		return false;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	return Part.bUnrigged && !Part.bAutoRigged && Part.bIncluded && Part.Slot.Equals(MantleUnriggedAsIsPrivate::AsIsAvatarSlot, ESearchCase::CaseSensitive)
		&& (Part.StaticSource.IsValid() || Part.Original.IsValid());
}

bool FMantleUnriggedAsIs::TransferWeights(const TConstArrayView<FVector3f> Points, const FMantleAutoRigSkin& Skin, const int32 HeadBoneIndex,
	const double HeadCutZ, FMantleAutoRigWeights& Out, FString& OutError)
{
	using namespace MantleUnriggedAsIsPrivate;
	using namespace UE::Geometry;
	Out = FMantleAutoRigWeights();
	Out.NumInfluences = MaxInfluences;
	const int32 Slots = Skin.NumInfluences;
	if (Skin.Positions.IsEmpty() || Skin.Indices.Num() < 3 || Slots <= 0 || Skin.Bones.Num() != Skin.Positions.Num() * Slots
		|| Skin.Weights.Num() != Skin.Bones.Num())
	{
		OutError = TEXT("the body has no skin to take weights from");
		return false;
	}

	// The skin as a surface to search: one mesh vertex per skin vertex (ids in order), a triangle the mesh can't take as it is
	// (it would make an edge with three triangles) on copies of its corners, so no triangle is lost.
	FDynamicMesh3 Surface;
	TArray<int32> SkinVertexOf;
	SkinVertexOf.Reserve(Skin.Positions.Num());
	for (int32 Vertex = 0; Vertex < Skin.Positions.Num(); ++Vertex)
	{
		Surface.AppendVertex(FVector3d(Skin.Positions[Vertex]));
		SkinVertexOf.Add(Vertex);
	}
	for (int32 First = 0; First + 2 < Skin.Indices.Num(); First += 3)
	{
		const int32 A = Skin.Indices[First];
		const int32 B = Skin.Indices[First + 1];
		const int32 C = Skin.Indices[First + 2];
		if (!Skin.Positions.IsValidIndex(A) || !Skin.Positions.IsValidIndex(B) || !Skin.Positions.IsValidIndex(C) || A == B || B == C || A == C)
		{
			continue;
		}
		if (Surface.AppendTriangle(A, B, C) >= 0)
		{
			continue;
		}
		const int32 CopyA = Surface.AppendVertex(FVector3d(Skin.Positions[A]));
		const int32 CopyB = Surface.AppendVertex(FVector3d(Skin.Positions[B]));
		const int32 CopyC = Surface.AppendVertex(FVector3d(Skin.Positions[C]));
		SkinVertexOf.Add(A);
		SkinVertexOf.Add(B);
		SkinVertexOf.Add(C);
		Surface.AppendTriangle(CopyA, CopyB, CopyC);
	}
	if (Surface.TriangleCount() == 0)
	{
		OutError = TEXT("the body has no skin to take weights from");
		return false;
	}
	const FDynamicMeshAABBTree3 Tree(&Surface, /*bAutoBuild*/ true);

	Out.Bones.Reserve(Points.Num() * MaxInfluences);
	Out.Weights.Reserve(Points.Num() * MaxInfluences);
	TArray<TPair<int32, double>> Blend;
	double TotalCm = 0.0;
	for (const FVector3f& Point : Points)
	{
		Blend.Reset();
		if (HeadBoneIndex != INDEX_NONE && Point.Z > HeadCutZ)
		{
			// Above the neck: the head alone (the body's skin ends at the neck; the face has no jaw to follow).
			Blend.Emplace(HeadBoneIndex, 1.0);
			++Out.HeadPoints;
		}
		else
		{
			double DistanceSquared = 0.0;
			const int32 Triangle = Tree.FindNearestTriangle(FVector3d(Point), DistanceSquared);
			if (Triangle < 0)
			{
				OutError = TEXT("a point of the model found no skin near it");
				return false;
			}
			const FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Surface, Triangle, FVector3d(Point));
			const FIndex3i Corners = Surface.GetTriangle(Triangle);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const double Share = Query.TriangleBaryCoords[Corner];
				const int32 SkinVertex = SkinVertexOf[Corners[Corner]];
				for (int32 Slot = 0; Slot < Slots; ++Slot)
				{
					const int32 Index = SkinVertex * Slots + Slot;
					if (Skin.Weights[Index] > 0.0f && Share > 0.0)
					{
						AddTo(Blend, Skin.Bones[Index], Share * Skin.Weights[Index]);
					}
				}
			}
			const double Distance = FMath::Sqrt(FMath::Max(0.0, DistanceSquared));
			TotalCm += Distance;
			Out.MaxDistanceCm = FMath::Max(Out.MaxDistanceCm, Distance);
			Out.FarPoints += Distance > FarCm ? 1 : 0;
			++Out.SkinPoints;
		}
		if (!KeepHeaviest(Blend, MaxInfluences))
		{
			OutError = TEXT("the body's skin near a point of the model has no weights");
			return false;
		}
		for (int32 Slot = 0; Slot < MaxInfluences; ++Slot)
		{
			Out.Bones.Add(Blend.IsValidIndex(Slot) ? Blend[Slot].Key : 0);
			Out.Weights.Add(Blend.IsValidIndex(Slot) ? static_cast<float>(Blend[Slot].Value) : 0.0f);
		}
	}
	Out.MeanDistanceCm = Out.SkinPoints > 0 ? TotalCm / Out.SkinPoints : 0.0;
	return true;
}

bool FMantleUnriggedAsIs::ReadSkin(const USkeletalMesh& Mesh, const FReferenceSkeleton& Target, FMantleAutoRigSkin& InOut, FString& OutError)
{
	const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
	if (!Imported || Imported->LODModels.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s has no geometry to read"), *Mesh.GetName());
		return false;
	}
	if (InOut.NumInfluences <= 0)
	{
		InOut.NumInfluences = MAX_TOTAL_INFLUENCES;
	}
	// Its bones onto Target's by name; a bone Target lacks (a face's FACIAL_ joints) onto its nearest ancestor Target has.
	const FReferenceSkeleton& Own = Mesh.GetRefSkeleton();
	TArray<int32> ToTarget;
	ToTarget.Init(INDEX_NONE, Own.GetRawBoneNum());
	for (int32 Bone = 0; Bone < Own.GetRawBoneNum(); ++Bone)
	{
		for (int32 Walk = Bone; Walk != INDEX_NONE && ToTarget[Bone] == INDEX_NONE; Walk = Own.GetRawParentIndex(Walk))
		{
			ToTarget[Bone] = Target.FindRawBoneIndex(Own.GetRawRefBoneInfo()[Walk].Name);
		}
	}
	const FSkeletalMeshLODModel& LOD = Imported->LODModels[0];
	const int32 Base = InOut.Positions.Num();
	int32 Read = 0;
	TArray<TPair<int32, float>> Influences;
	for (const FSkelMeshSection& Section : LOD.Sections)
	{
		for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
		{
			Influences.Reset();
			for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
			{
				const int32 Weight = Vertex.InfluenceWeights[Influence];
				const int32 Local = Vertex.InfluenceBones[Influence];
				const int32 Bone = Section.BoneMap.IsValidIndex(Local) ? static_cast<int32>(Section.BoneMap[Local]) : INDEX_NONE;
				if (Weight > 0 && ToTarget.IsValidIndex(Bone) && ToTarget[Bone] != INDEX_NONE)
				{
					Influences.Emplace(ToTarget[Bone], static_cast<float>(Weight) / 65535.0f);
				}
			}
			InOut.AddVertex(Vertex.Position, Influences);
			++Read;
		}
	}
	// The index buffer counts vertices across every section, in their order.
	for (const FSkelMeshSection& Section : LOD.Sections)
	{
		const int32 End = static_cast<int32>(Section.BaseIndex + Section.NumTriangles * 3);
		for (int32 Index = static_cast<int32>(Section.BaseIndex); Index < End && LOD.IndexBuffer.IsValidIndex(Index); ++Index)
		{
			const int32 Vertex = static_cast<int32>(LOD.IndexBuffer[Index]);
			InOut.Indices.Add(Vertex < Read ? Base + Vertex : INDEX_NONE);
		}
	}
	if (Read == 0)
	{
		OutError = FString::Printf(TEXT("%s has no vertices"), *Mesh.GetName());
		return false;
	}
	return true;
}

bool FMantleUnriggedAsIs::MakeSkinnedMesh(UStaticMesh& Posed, USkeletalMesh& Body, USkeletalMesh* Head, const FString& Folder, const FString& InName,
	FMantleAutoRigResult& OutResult, FString& OutError)
{
	using namespace MantleUnriggedAsIsPrivate;
	using namespace UE::Geometry;
	OutResult = FMantleAutoRigResult();
	const FString Name = FMantleMetaHumanSteps::AssetTag(InName);

	// The posed model's LOD 0 source, read with Geometry Script's default options: material IDs are its slot numbers.
	UDynamicMesh* Geometry = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> GeometryHeld(Geometry);
	FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
	FGeometryScriptMeshReadLOD ReadLOD;
	ReadLOD.LODType = EGeometryScriptLODType::SourceModel;
	ReadLOD.LODIndex = 0;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(&Posed, Geometry, CopyOptions, ReadLOD, Outcome, /*bUseSectionMaterials*/ false);
	if (Outcome != EGeometryScriptOutcomePins::Success || Geometry->GetTriangleCount() == 0)
	{
		OutError = FString::Printf(TEXT("%s's geometry could not be read"), *Posed.GetName());
		return false;
	}

	// The skin: the A-pose body, and the head on it (its neck and face joints onto the body's neck and head).
	TArray<USkinnedAsset*> Compiling = { &Body };
	if (Head)
	{
		Compiling.Add(Head);
	}
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	const FReferenceSkeleton& Reference = Body.GetRefSkeleton();
	const int32 HeadBoneIndex = Reference.FindRawBoneIndex(HeadBone);
	if (HeadBoneIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s has no %s bone"), *Body.GetName(), *HeadBone.ToString());
		return false;
	}
	OutResult.HeadCutZ = FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, HeadBoneIndex).GetLocation().Z;
	FMantleAutoRigSkin Skin;
	Skin.NumInfluences = MAX_TOTAL_INFLUENCES;
	if (!ReadSkin(Body, Reference, Skin, OutError))
	{
		return false;
	}
	FString HeadError;
	if (Head && !ReadSkin(*Head, Reference, Skin, HeadError))
	{
		// The body alone still carries the model; the cut puts the head on its bone.
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_ASISRIG the head's skin was not read (%s): the neck takes the body's weights"), *HeadError);
	}

	// The model's points, in vertex id order.
	TArray<FVector3f> Points;
	TArray<int32> PointOf;
	Geometry->ProcessMesh([&Points, &PointOf](const FDynamicMesh3& Read)
	{
		PointOf.Init(INDEX_NONE, Read.MaxVertexID());
		for (const int32 Vertex : Read.VertexIndicesItr())
		{
			PointOf[Vertex] = Points.Add(FVector3f(Read.GetVertex(Vertex)));
		}
	});

	// The conform fits the body to the model where it stands, so the model is weighted where it is: closing the gap to the body's
	// skin pulled it toward its hair and clothing (RoninGirl rose 6 cm, the weightless Girl sank 5 cm), and its bounds are no
	// guide either (RoninGirl's backpack moves their middle 11 cm back).
	{
		FBox3f ModelBox(ForceInit);
		FBox3f BodyBox(ForceInit);
		for (const FVector3f& Point : Points)
		{
			ModelBox += Point;
		}
		for (const FVector3f& Position : Skin.Positions)
		{
			BodyBox += Position;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASISRIG bounds: model %s, body %s"), *ModelBox.ToString(), *BodyBox.ToString());
	}

	// The hands onto the model's: the conform's body holds its hands where MetaHuman's A-pose puts them, 11 to 12 cm in front of
	// the model's on the three test models (its finger joints sit inside its own fingers, not the model's), so the model's fingers
	// took finger weights from bones 7 to 15 cm away and curled into claws. Each body hand, its fingers and the skin on them are laid
	// onto the model's hand (FitRigid, from the points that first took the hand's weights), the skeleton's hand joint with them.
	FReferenceSkeleton Rest = Reference;
	TArray<FTransform> RefPose;
	FAnimationRuntime::FillUpComponentSpaceTransforms(Reference, Reference.GetRawRefBonePose(), RefPose);
	TArray<FTransform> Pose = RefPose;
	if (!TransferWeights(Points, Skin, HeadBoneIndex, OutResult.HeadCutZ, OutResult.Weights, OutError))
	{
		return false;
	}
	for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
	{
		const int32 Hand = Reference.FindRawBoneIndex(FName(*FString::Printf(TEXT("hand_%s"), Side)));
		const int32 Arm = Hand == INDEX_NONE ? INDEX_NONE : Reference.GetRawParentIndex(Hand);
		if (Arm == INDEX_NONE)
		{
			continue;
		}
		TArray<bool> InHand;
		InHand.Init(false, Reference.GetRawBoneNum());
		for (int32 Bone = 0; Bone < Reference.GetRawBoneNum(); ++Bone)
		{
			for (int32 Walk = Bone; Walk != INDEX_NONE && !InHand[Bone]; Walk = Reference.GetRawParentIndex(Walk))
			{
				InHand[Bone] = Walk == Hand;
			}
		}
		// The body's hand: its skin mostly on the hand and fingers. The model's: the points that took mostly the hand's weights.
		auto HandShare = [&InHand](const TArray<int32>& Bones, const TArray<float>& Weights, const int32 Slots, const int32 Vertex)
		{
			float Share = 0.0f;
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				const int32 Index = Vertex * Slots + Slot;
				Share += InHand.IsValidIndex(Bones[Index]) && InHand[Bones[Index]] ? Weights[Index] : 0.0f;
			}
			return Share;
		};
		TArray<FVector3f> BodyHand;
		FVector3f BodyCentre = FVector3f::ZeroVector;
		for (int32 Vertex = 0; Vertex < Skin.Positions.Num(); ++Vertex)
		{
			if (HandShare(Skin.Bones, Skin.Weights, Skin.NumInfluences, Vertex) >= 0.5f)
			{
				BodyHand.Add(Skin.Positions[Vertex]);
				BodyCentre += Skin.Positions[Vertex];
			}
		}
		FVector3f ModelCentre = FVector3f::ZeroVector;
		int32 ModelCount = 0;
		for (int32 Point = 0; Point < Points.Num(); ++Point)
		{
			if (HandShare(OutResult.Weights.Bones, OutResult.Weights.Weights, OutResult.Weights.NumInfluences, Point) >= 0.5f)
			{
				ModelCentre += Points[Point];
				++ModelCount;
			}
		}
		if (BodyHand.IsEmpty() || ModelCount == 0)
		{
			continue;
		}
		BodyCentre /= BodyHand.Num();
		ModelCentre /= ModelCount;
		// The fit's targets: the model's points near its hand; its sources: at most HandFitSamples of the body hand's skin.
		TArray<FVector3f> Near;
		for (const FVector3f& Point : Points)
		{
			if (FVector3f::Dist(Point, ModelCentre) <= HandFitRadiusCm)
			{
				Near.Add(Point);
			}
		}
		TArray<FVector3f> Sources;
		const int32 Stride = FMath::Max(1, BodyHand.Num() / HandFitSamples);
		for (int32 Index = 0; Index < BodyHand.Num(); Index += Stride)
		{
			Sources.Add(BodyHand[Index]);
		}
		// Where it stands, then laid on from the two hands' centres.
		double StayCm = 0.0;
		double Unused = 0.0;
		double FromCm = 0.0;
		double FitCm = 0.0;
		FitRigid(Sources, Near, FTransform::Identity, 0, StayCm, Unused);
		const FTransform Fit = FitRigid(Sources, Near, FTransform(FVector(ModelCentre - BodyCentre)), HandFitRounds, FromCm, FitCm);
		const double MovedCm = FVector::Dist(Fit.TransformPosition(RefPose[Hand].GetLocation()), RefPose[Hand].GetLocation());
		const double TurnDegrees = FMath::RadiansToDegrees(Fit.GetRotation().GetAngle());
		const bool bTake = FitCm < StayCm && MovedCm <= HandFitMaxCm && TurnDegrees <= HandFitMaxDegrees;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASISRIG hand_%s onto the model's: %s (its skin %.2f cm from the model's where it stood, %.2f cm laid on; the hand joint moved %.1f cm, turned %.1f degrees; %d skin points onto %d model points)"),
			Side, bTake ? TEXT("laid on") : TEXT("left where it stood"), StayCm, FitCm, MovedCm, TurnDegrees, Sources.Num(), Near.Num());
		if (!bTake)
		{
			continue;
		}
		for (int32 Bone = 0; Bone < Reference.GetRawBoneNum(); ++Bone)
		{
			if (InHand[Bone])
			{
				Pose[Bone] = RefPose[Bone] * Fit;
			}
		}
		FReferenceSkeletonModifier Modifier(Rest, nullptr);
		Modifier.UpdateRefPoseTransform(Hand, Pose[Hand].GetRelativeTransform(RefPose[Arm]));
	}
	MoveByWeights(Skin.Positions, Skin.NumInfluences, Skin.Bones, Skin.Weights, RefPose, Pose, /*bInverse*/ false);
	if (!TransferWeights(Points, Skin, HeadBoneIndex, OutResult.HeadCutZ, OutResult.Weights, OutError))
	{
		return false;
	}
	// Each hand's finger joints against the body's own finger skin and the model's, for the record (whether the hands line up).
	{
		auto JointAt = [&Pose](const int32 Bone) { return FVector3f(Pose[Bone].GetLocation()); };
		auto DominantOf = [](const TArray<int32>& Bones, const TArray<float>& Weights, const int32 Slots, const int32 Vertex)
		{
			int32 Best = INDEX_NONE;
			float BestWeight = 0.0f;
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				const int32 Index = Vertex * Slots + Slot;
				if (Weights.IsValidIndex(Index) && Weights[Index] > BestWeight)
				{
					Best = Bones[Index];
					BestWeight = Weights[Index];
				}
			}
			return Best;
		};
		auto V = [](const FVector3f& P) { return FString::Printf(TEXT("(%.1f %.1f %.1f)"), P.X, P.Y, P.Z); };
		for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
		{
			const int32 Hand = Reference.FindRawBoneIndex(FName(*FString::Printf(TEXT("hand_%s"), Side)));
			if (Hand == INDEX_NONE)
			{
				continue;
			}
			TArray<bool> InHand;
			InHand.Init(false, Reference.GetRawBoneNum());
			for (int32 Bone = 0; Bone < Reference.GetRawBoneNum(); ++Bone)
			{
				for (int32 Walk = Bone; Walk != INDEX_NONE && !InHand[Bone]; Walk = Reference.GetRawParentIndex(Walk))
				{
					InHand[Bone] = Walk == Hand;
				}
			}
			FString Line = FString::Printf(TEXT("hand_%s joint %s"), Side, *V(JointAt(Hand)));
			for (const TCHAR* Finger : { TEXT("thumb_02"), TEXT("index_01"), TEXT("index_02"), TEXT("middle_02"), TEXT("pinky_02") })
			{
				const int32 Bone = Reference.FindRawBoneIndex(FName(*FString::Printf(TEXT("%s_%s"), Finger, Side)));
				if (Bone == INDEX_NONE)
				{
					continue;
				}
				const FVector3f Joint = JointAt(Bone);
				FVector3f BodySum = FVector3f::ZeroVector;
				FVector3f ModelSum = FVector3f::ZeroVector;
				int32 BodyCount = 0;
				int32 ModelCount = 0;
				float BodyNearest = TNumericLimits<float>::Max();
				float ModelNearest = TNumericLimits<float>::Max();
				for (int32 Vertex = 0; Vertex < Skin.Positions.Num(); ++Vertex)
				{
					BodyNearest = FMath::Min(BodyNearest, FVector3f::Dist(Joint, Skin.Positions[Vertex]));
					if (DominantOf(Skin.Bones, Skin.Weights, Skin.NumInfluences, Vertex) == Bone)
					{
						BodySum += Skin.Positions[Vertex];
						++BodyCount;
					}
				}
				for (int32 Point = 0; Point < Points.Num(); ++Point)
				{
					ModelNearest = FMath::Min(ModelNearest, FVector3f::Dist(Joint, Points[Point]));
					if (OutResult.Weights.Dominant(Point) == Bone)
					{
						ModelSum += Points[Point];
						++ModelCount;
					}
				}
				const FVector3f BodyCentre = BodyCount > 0 ? BodySum / BodyCount : FVector3f::ZeroVector;
				const FVector3f ModelCentre = ModelCount > 0 ? ModelSum / ModelCount : FVector3f::ZeroVector;
				Line += FString::Printf(TEXT("; %s joint %s: body skin on it %d centred %s (%.1f cm from the joint), nearest body skin %.1f cm; model points on it %d centred %s (%.1f cm), nearest model point %.1f cm"),
					Finger, *V(Joint), BodyCount, *V(BodyCentre), BodyCount > 0 ? FVector3f::Dist(Joint, BodyCentre) : -1.0f, BodyNearest, ModelCount, *V(ModelCentre),
					ModelCount > 0 ? FVector3f::Dist(Joint, ModelCentre) : -1.0f, ModelNearest);
			}
			FVector3f BodySum = FVector3f::ZeroVector;
			FVector3f ModelSum = FVector3f::ZeroVector;
			int32 BodyCount = 0;
			int32 ModelCount = 0;
			for (int32 Vertex = 0; Vertex < Skin.Positions.Num(); ++Vertex)
			{
				const int32 Bone = DominantOf(Skin.Bones, Skin.Weights, Skin.NumInfluences, Vertex);
				if (InHand.IsValidIndex(Bone) && InHand[Bone])
				{
					BodySum += Skin.Positions[Vertex];
					++BodyCount;
				}
			}
			for (int32 Point = 0; Point < Points.Num(); ++Point)
			{
				const int32 Bone = OutResult.Weights.Dominant(Point);
				if (InHand.IsValidIndex(Bone) && InHand[Bone])
				{
					ModelSum += Points[Point];
					++ModelCount;
				}
			}
			Line += FString::Printf(TEXT("; hand and fingers: body skin %d centred %s, model points %d centred %s"), BodyCount,
				*V(BodyCount > 0 ? BodySum / BodyCount : FVector3f::ZeroVector), ModelCount, *V(ModelCount > 0 ? ModelSum / ModelCount : FVector3f::ZeroVector));
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASISRIG handfit %s"), *Line);
		}
	}
	// A finger's skin follows one finger: closest points between fingers blend neighbours, which stretch when the fingers curl.
	{
		TArray<FName> BoneNames;
		TArray<int32> BoneParents;
		TArray<FVector3f> BoneJoints;
		for (int32 Bone = 0; Bone < Reference.GetRawBoneNum(); ++Bone)
		{
			BoneNames.Add(Reference.GetRawRefBoneInfo()[Bone].Name);
			BoneParents.Add(Reference.GetRawParentIndex(Bone));
			BoneJoints.Add(FVector3f(Pose[Bone].GetLocation()));
		}
		OutResult.Fingers = KeepOneFinger(Points, BoneNames, BoneParents, BoneJoints, OutResult.Weights);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASISRIG fingers: %s"), *OutResult.Fingers.ToString());
	}

	// On the floor: the game stands an avatar's root on the floor and plays it as tall as its top (the export's standing height), so
	// the model's lowest point goes to Z 0, its skeleton with it (the bones under the root move as the skin does, the binding stays).
	{
		float FloorZ = TNumericLimits<float>::Max();
		for (const FVector3f& Point : Points)
		{
			FloorZ = FMath::Min(FloorZ, Point.Z);
		}
		OutResult.FloorShiftCm = FMath::Abs(FloorZ) > FloorToleranceCm ? -FloorZ : 0.0;
		if (OutResult.FloorShiftCm != 0.0)
		{
			for (FVector3f& Point : Points)
			{
				Point.Z += static_cast<float>(OutResult.FloorShiftCm);
			}
			const FTransform Root = FAnimationRuntime::GetComponentSpaceTransformRefPose(Reference, 0);
			const FVector Shift = Root.InverseTransformVectorNoScale(FVector(0.0, 0.0, OutResult.FloorShiftCm));
			FReferenceSkeletonModifier Modifier(Rest, nullptr);
			for (int32 Bone = 1; Bone < Reference.GetRawBoneNum(); ++Bone)
			{
				if (Reference.GetRawParentIndex(Bone) == 0)
				{
					FTransform Local = Rest.GetRawRefBonePose()[Bone];
					Local.AddToTranslation(Shift);
					Modifier.UpdateRefPoseTransform(Bone, Local);
				}
			}
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASISRIG %s: head cut at %.2f cm; %s; feet put on the floor (moved %.2f cm up, the skeleton with them)"), *Posed.GetName(),
		OutResult.HeadCutZ, *OutResult.Weights.ToString(), OutResult.FloorShiftCm);

	// The slots: the posed model's own materials, by unique slot name (the build matches sections to slots by name).
	const TArray<FStaticMaterial>& StaticMaterials = Posed.GetStaticMaterials();
	TArray<FName> SlotNames = UniqueSlotNames(StaticMaterials);
	if (SlotNames.IsEmpty())
	{
		SlotNames.Add(FName(TEXT("Slot_0")));
	}
	TArray<FSkeletalMaterial> Materials;
	for (int32 Slot = 0; Slot < SlotNames.Num(); ++Slot)
	{
		UMaterialInterface* Material = StaticMaterials.IsValidIndex(Slot) ? StaticMaterials[Slot].MaterialInterface.Get() : nullptr;
		Materials.Add(FSkeletalMaterial(Material, /*bInEnableShadowCasting*/ true, /*bInRecomputeTangent*/ false, SlotNames[Slot], SlotNames[Slot]));
	}

	// The description: the body's bones, one vertex per model vertex with its weights, one vertex instance per triangle corner (the
	// build welds the identical ones), a polygon group per slot used.
	FMeshDescription Description;
	FSkeletalMeshAttributes Attributes(Description);
	Attributes.Register();
	bool bNormals = false;
	Geometry->ProcessMesh([&](const FDynamicMesh3& Read)
	{
		const FDynamicMeshAttributeSet* Overlays = Read.HasAttributes() ? Read.Attributes() : nullptr;
		const FDynamicMeshNormalOverlay* NormalOverlay = Overlays ? Overlays->PrimaryNormals() : nullptr;
		const FDynamicMeshColorOverlay* ColorOverlay = Overlays && Overlays->HasPrimaryColors() ? Overlays->PrimaryColors() : nullptr;
		const FDynamicMeshMaterialAttribute* MaterialIDs = Overlays && Overlays->HasMaterialID() ? Overlays->GetMaterialID() : nullptr;
		const int32 NumUVs = FMath::Clamp(Overlays ? Overlays->NumUVLayers() : 0, 1, static_cast<int32>(MAX_TEXCOORDS));
		bNormals = NormalOverlay != nullptr;
		Attributes.GetVertexInstanceUVs().SetNumChannels(NumUVs);
		{
			FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
			FSkeletalMeshAttributes::FBoneParentIndexAttributesRef BoneParents = Attributes.GetBoneParentIndices();
			FSkeletalMeshAttributes::FBonePoseAttributesRef BonePoses = Attributes.GetBonePoses();
			for (int32 Bone = 0; Bone < Rest.GetRawBoneNum(); ++Bone)
			{
				const FBoneID Id = Attributes.CreateBone();
				BoneNames.Set(Id, Rest.GetRawRefBoneInfo()[Bone].Name);
				BoneParents.Set(Id, Rest.GetRawParentIndex(Bone));
				BonePoses.Set(Id, Rest.GetRawRefBonePose()[Bone]);
			}
		}
		const TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		const TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		const TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		const TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		FSkinWeightsVertexAttributesRef SkinWeights = Attributes.GetVertexSkinWeights();
		const FMantleAutoRigWeights& Weights = OutResult.Weights;

		TArray<FVertexID> VertexOf;
		VertexOf.Init(FVertexID(INDEX_NONE), Read.MaxVertexID());
		TArray<FBoneWeight> Influence;
		for (const int32 Vertex : Read.VertexIndicesItr())
		{
			const int32 Point = PointOf[Vertex];
			const FVertexID Made = Description.CreateVertex();
			Positions.Set(Made, Points[Point]);
			Influence.Reset();
			for (int32 Slot = 0; Slot < Weights.NumInfluences; ++Slot)
			{
				const int32 Index = Point * Weights.NumInfluences + Slot;
				if (Weights.Weights[Index] > 0.0f)
				{
					Influence.Add(FBoneWeight(static_cast<FBoneIndexType>(Weights.Bones[Index]), Weights.Weights[Index]));
				}
			}
			SkinWeights.Set(Made, TArrayView<const FBoneWeight>(Influence));
			VertexOf[Vertex] = Made;
		}

		TMap<int32, FPolygonGroupID> Groups;
		auto GroupOf = [&](const int32 MaterialID)
		{
			const int32 Slot = SlotNames.IsValidIndex(MaterialID) ? MaterialID : 0;
			if (const FPolygonGroupID* Found = Groups.Find(Slot))
			{
				return *Found;
			}
			const FPolygonGroupID Made = Description.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames().Set(Made, SlotNames[Slot]);
			Groups.Add(Slot, Made);
			return Made;
		};
		for (const int32 Triangle : Read.TriangleIndicesItr())
		{
			const FIndex3i Corners = Read.GetTriangle(Triangle);
			FVertexInstanceID Instances[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVertexInstanceID Instance = Description.CreateVertexInstance(VertexOf[Corners[Corner]]);
				if (NormalOverlay && NormalOverlay->IsSetTriangle(Triangle))
				{
					Normals.Set(Instance, NormalOverlay->GetElement(NormalOverlay->GetTriangle(Triangle)[Corner]));
				}
				Signs.Set(Instance, 1.0f);
				for (int32 Channel = 0; Channel < NumUVs; ++Channel)
				{
					const FDynamicMeshUVOverlay* UVOverlay = Overlays && Channel < Overlays->NumUVLayers() ? Overlays->GetUVLayer(Channel) : nullptr;
					if (UVOverlay && UVOverlay->IsSetTriangle(Triangle))
					{
						UVs.Set(Instance, Channel, UVOverlay->GetElement(UVOverlay->GetTriangle(Triangle)[Corner]));
					}
				}
				Colors.Set(Instance, ColorOverlay && ColorOverlay->IsSetTriangle(Triangle)
					? ColorOverlay->GetElement(ColorOverlay->GetTriangle(Triangle)[Corner]) : FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
				Instances[Corner] = Instance;
			}
			Description.CreateTriangle(GroupOf(MaterialIDs ? MaterialIDs->GetValue(Triangle) : 0), TArrayView<const FVertexInstanceID>(Instances, 3));
		}
	});

	FBox Bounds(ForceInit);
	for (const FVector3f& Point : Points)
	{
		Bounds += FVector(Point);
	}

	// The mesh on the body's reference skeleton (its joints where the conform put them) and its skeleton (Epic's MetaHuman body
	// skeleton, which the clips play on); one LOD, every triangle kept, tangents from the UVs.
	const FString PackageName = FMantleMetaHumanSteps::UniquePackageName(Folder, FString::Printf(TEXT("SK_%s_AsIs"), *Name));
	const FString AssetName = FPackageName::GetShortName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	USkeletalMesh* Mesh = Package ? NewObject<USkeletalMesh>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("could not create %s"), *PackageName);
		return false;
	}
	OutResult.Created.Add(Mesh);
	Mesh->PreEditChange(nullptr);
	Mesh->SetRefSkeleton(Rest);
	Mesh->GetRefBasesInvMatrix().Reset();
	Mesh->CalculateInvRefMatrices();
	Mesh->SetMaterials(Materials);
	FSkeletalMeshLODInfo& LODInfo = Mesh->AddLODInfo();
	LODInfo.BuildSettings.bRecomputeNormals = !bNormals;
	LODInfo.BuildSettings.bRecomputeTangents = true;
	LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	Mesh->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel);
	Mesh->CreateMeshDescription(0, MoveTemp(Description));
	Mesh->CommitMeshDescription(0);
	Mesh->SetImportedBounds(FBoxSphereBounds(Bounds));
	USkeleton* Skeleton = Body.GetSkeleton();
	const bool bTransientSkeleton = !Skeleton || Skeleton->HasAnyFlags(RF_Transient) || Skeleton->GetPackage() == GetTransientPackage();
	if (!bTransientSkeleton)
	{
		Mesh->SetSkeleton(Skeleton);
	}
	Mesh->PostEditChange();
	TArray<USkinnedAsset*> Built = { Mesh };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Built);
	FAssetRegistryModule::AssetCreated(Mesh);
	if (bTransientSkeleton)
	{
		// The export's skeleton lives in memory only: the mesh gets a saved one of the same bones, next to it.
		const FString SkeletonPackageName = FMantleMetaHumanSteps::UniquePackageName(Folder, AssetName + TEXT("_Skeleton"));
		UPackage* SkeletonPackage = CreatePackage(*SkeletonPackageName);
		USkeleton* Made = SkeletonPackage ? NewObject<USkeleton>(SkeletonPackage, FName(*FPackageName::GetShortName(SkeletonPackageName)), RF_Public | RF_Standalone) : nullptr;
		if (Made)
		{
			OutResult.Created.Add(Made);
		}
		if (!Made || !Made->MergeAllBonesToBoneTree(Mesh, false))
		{
			OutError = TEXT("the skinned mesh's bones could not make a skeleton");
			return false;
		}
		Mesh->SetSkeleton(Made);
		Made->SetPreviewMesh(Mesh);
		Made->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Made);
	}
	Mesh->MarkPackageDirty();
	OutResult.Mesh = Mesh;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASISRIG made %s: %d vertices, %d triangles, %d slots, on %s's skeleton (%d bones)"), *Mesh->GetPathName(),
		Points.Num(), Geometry->GetTriangleCount(), Materials.Num(), *Body.GetName(), Reference.GetRawBoneNum());
	return true;
}

void FMantleUnriggedAsIsFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		if (!FMantleUnriggedAsIs::IsUnriggedAsIs(Context, PartIndex))
		{
			continue;
		}
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Problem = TEXT("it has no rig to keep");
		// The conform's own refusals, in its words.
		if (GUsingNullRHI)
		{
			Fix.Action = TEXT("Can't: fitting a skeleton takes a front portrait of the face, which needs a renderer, and this editor runs without one.");
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			continue;
		}
		if (!FSlateApplication::IsInitialized())
		{
			Fix.Action = TEXT("Can't: MetaHuman's geometry export needs the editor's UI, and this editor runs without it.");
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			continue;
		}
		Fix.Action = FMantleUnriggedAsIs::PlainMessage()
			+ TEXT(" Epic's conform places the skeleton, guided by a front portrait of the face (it needs a human-like face looking forward). Everything above the neck follows the head; hair, capes and skirts follow the nearest body bone. The face doesn't animate (no jaw).");
	}
}

bool FMantleUnriggedAsIsFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !FMantleUnriggedAsIs::IsUnriggedAsIs(Context, Fix.PartIndex))
	{
		OutError = TEXT("there is no model without a rig to fit");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	const FString Name = FMantleMetaHumanSteps::AssetTag(Part.Name);
	const FString WorkFolder = Context.OutputFolder / TEXT("Work");
	const FString CharacterFolder = Context.OutputFolder / Name;
	// Nothing of Epic's, MetaHuman Creator's or the source's may change; the step writes only into <output> (Work, the
	// character's folder and the skinned mesh).
	const TArray<FString> Roots = FMantleMetaHumanSteps::ProtectedRoots(Context, Fix.PartIndex);
	const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Roots, Context.OutputFolder);
	TArray<TWeakObjectPtr<UObject>> Made;

	// 1. The model bent into the conform's pose and scaled to the default height (the unrigged Convert route's first step).
	FMantleUnriggedBodyResult Posed;
	bool bDone = FMantleUnriggedBody::MakePosedMesh(Part, WorkFolder, Part.Name, FMantleAvatarHeight::TargetCm(), Posed, OutError);
	Made.Append(Posed.Created);
	// 2. Epic's conform: a MetaHuman Character fitted inside it, its A-pose body (Epic's skeleton, the joints in the model) exported.
	FMantleMetaHumanConformResult Conformed;
	if (bDone)
	{
		bDone = FMantleMetaHumanConform::Conform(*Posed.PosedMesh.Get(), TOptional<FVector>(), CharacterFolder, Name, Conformed, OutError);
		Made.Append(Conformed.Created);
		if (bDone && !Conformed.APoseBody.IsValid())
		{
			OutError = TEXT("MetaHuman's conform gave no A-pose body");
			bDone = false;
		}
	}
	// 3. The model skinned to that body.
	FMantleAutoRigResult Rigged;
	if (bDone)
	{
		bDone = FMantleUnriggedAsIs::MakeSkinnedMesh(*Posed.PosedMesh.Get(), *Conformed.APoseBody.Get(), Conformed.APoseHead.Get(), Context.OutputFolder, Part.Name,
			Rigged, OutError);
		Made.Append(Rigged.Created);
	}
	// Checked whether the step finished or not: one that stopped half-way may have changed something too.
	FString Dirtied;
	if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, Context.OutputFolder, DirtyBefore, TEXT("The auto-rig"), Dirtied))
	{
		OutError = bDone ? Dirtied : FString::Printf(TEXT("%s. %s"), *OutError, *Dirtied);
		bDone = false;
	}
	if (!bDone)
	{
		// The fixer contract: a failed Apply leaves nothing behind and the context as it was.
		FMantleMetaHumanSteps::Discard(Made);
		return false;
	}
	for (const TWeakObjectPtr<UObject>& Created : Made)
	{
		Context.CreatedAssets.AddUnique(Created);
	}
	// The part is now a skinned mesh on Epic's body skeleton: the As-is fixers after this take it as any As-is body.
	Part.Mesh = Rigged.Mesh;
	Part.SkeletonId = TEXT("metahuman");
	Part.bAutoRigged = true;
	const USkeletalMesh* Mesh = Rigged.Mesh.Get();
	const FMantleAutoRigWeights& Weights = Rigged.Weights;
	Context.Notes.Add(FString::Printf(TEXT("%s: %s Made %s on Epic's MetaHuman body skeleton: %d of %d points weighted from the fitted body's skin (on average %.1f cm from it; %d more than %.0f cm away, such as hair or a cape, follow their nearest bone), %d above the neck on the head. Scaled by %.4f from %.1f cm toward the default height. The face doesn't animate (no jaw)."),
		*Part.Name, *FMantleUnriggedAsIs::PlainMessage(), Mesh ? *Mesh->GetName() : TEXT("?"), Weights.SkinPoints, Weights.SkinPoints + Weights.HeadPoints,
		Weights.MeanDistanceCm, Weights.FarPoints, FMantleUnriggedAsIs::FarCm, Weights.HeadPoints, Posed.InputScale, Posed.HeightCm));
	return true;
}

void RegisterMantleUnriggedAsIsFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleUnriggedAsIsFixer>());
}
